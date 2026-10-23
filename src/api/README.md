# API implementation

`stream_person` in `agent_stream_triggers.cpp` is shared with `mcp_connection_streams.cpp` and
requires a present, enabled account (lake#951). Disabled/missing principals trigger idempotent
workspace cleanup; storage errors leave records pending for retry. No notifications, launches or
results are processed under a refused principal. Store creation guards also reject a context
reconstructed before offboarding. See [Clerk offboarding](../../docs/api/clerk.md#webhooks).
`export_capabilities.cpp` accepts portable archives only after the manifest verifier binds the
claimed source name to its key (LSR-088, lake#954). Imported forge/artifact claims carry untrusted
provenance, release evidence cannot satisfy local branch protection, and recreated grants use
the importer and freshly signed local attestations. See [export.md](../../docs/api/export.md).
`agent_capabilities.cpp` passes the authenticated acting identity separately to the agent signing
service. Customer write permission does not allow another dispatcher or a durable agent without
an active certificate for the customer. See [agent session keys](../../docs/api/agents.md).

`tunnel_clients.cpp` reads the tunnel client folder (allow-listed `zetta-tunnel-<platform>` files
with a `SHA256SUMS` line, and `VERSION`) and signs and checks their 15-minute links with
`signed_link_signature`, the HMAC the Hub's LFS upload links also use (lake#745).
`tunnel_client_capabilities.cpp` registers `tunnel_client_downloads` for any signed-in person, and
`http_server.cpp`'s `serve_tunnel_client` serves `GET /tunnel-clients/<platform>`. See
[tunnel-clients.md](../../docs/api/tunnel-clients.md).

`network_capabilities.cpp` defines fourteen account-authorized secure-networking operations once for
REST, OpenAPI and MCP. Desired state uses the network store; sessions and activity retain their
workspace-record boundary. Its list handlers authorize their scoped candidates, and
`agent_docs.cpp` places all fourteen in the Network group, and `tunnel_client_downloads` beside them. App credential rotation advances the
unchanged connection's desired generation and keeps the workspace-secret reference stable.
Recipient-only CSR issuance uses the
customer PKI, a current grant and ready relay trust; it is centrally audited and unmetered.
`ServerOptions.network_records` supplies session/activity reads, defaulting to an explicit
pending zetta#1281 response. See [scoped credentials](../../docs/api/scoped-credentials.md),
[tunnel credentials](../../docs/api/tunnel-grants.md) and [the API contract](../../docs/api/network.md).

**The console is the web app (lake#561).** The pages are `frontend/`, embedded in the binary and
answered for every browser page request by the `web_app` middleware (`web_app.cpp`,
[web-app.md](../../docs/ui/web-app.md)); they call only `/v1`. lake#561 retired the
server-rendered console: the page and deferred-panel handlers and every HTML form handler in
`http_server.cpp`, the renderers in `web_ui.cpp`, `web_ui_streams.cpp` and `web_ui_experiments.cpp`,
the `lake_workspace` cookie and the experiment chart assets. Where a paragraph below says a page
renders or posts something, the web app now does it through the capability named. `web_ui.cpp`
keeps the hosted identity-application pages, the stylesheet and fonts they link, a handler
failure's page, and the pure helpers capabilities use (`web_ui_escape`, `web_ui_space_label`,
`web_ui_source_units`). `agent_turn` checks a confirmed Application Agent or Experiments Agent
proposal against the stored one (`proposal_task_id`, `proposal_sha256`), which the form handlers did.

`store_owner.cpp` holds the exclusive owner lock on `LAKE_STORE_ROOT`, with a bounded wait for a
predecessor. `ApiServer::start` binds first; its owner thread takes the lock and opens the customer
stores (each with liquid's `exclusive_owner`), while the `store_root` middleware holds every request
but probes, metrics and static assets until the root is open and `/readyz` reports the hand-off.
`begin_drain` (at SIGTERM) refuses new workspace store requests and releases each workspace
store's lease once its admitted requests are done (lake#536), after it writes `.lake-owner.draining`
(`StoreOwnerLock::announce_draining`), the announcement a successor's pre-open waits for before it
takes a workspace lease; `stop` drains in-flight requests,
closes the stores and releases the lock (lake#467, lake#479,
[store-volume.md](../../docs/deployment/store-volume.md#the-hand-off)). A starting server opens the
stores and repositories of running switched workspaces in the background (`preopen_workspace_stores`);
once an attempt's acquire returns it checks the workspace again under the lane and closes the store
of one stopped, deleted or unswitched meanwhile, releasing its lease at once (lake#541).
lake#452: the `viewer` read labels the viewer's own space *Your workspaces* and workspaces by
name; `workspace_act create_project` says whether a name is missing or taken;
`create_app` requires `confirm_public` (or a person's acknowledgement on the confirmed
Application Agent turn) for a public app; `list_services` answers the page's `status` and
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
builds *Talk to* with `control::talk_to_agents`, which offers every library chat agent (lake#788). See
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
The retired content columns, and their copy, went with migration `0070`.

A GET shares authentication within the request. Data uses a projected dataset/database list from
`system.datasets`; dataset details own metadata and the bounded sample. `Server-Timing` reports
actual outbound calls and their sequential rounds, and names each call by its Zetta capability
(`zetta_<capability>`). Startup warms existing repository indexes for Metering without
caching usage. `metering_capabilities.cpp` is asynchronous: its summary reads every workspace's
datasets, runs, and repository transfer and build records concurrently (lake#348, lake#349), and
image bytes are counted from the tags in each routed workspace (`tenant_image_bytes`, lake#383), leaving out blobs already in a workspace store (`images_in_workspace_stores_bytes`, phase B). `metering_recorder.cpp` is the `MeteringRecorder`: the sync capabilities in
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

Request telemetry (decision 54 S9, lake#384) stays with the instance. `request_log.hpp` builds the
per-request stdout line from the route pattern and the captured UUID identifiers, never the raw
path; `http_server.cpp`'s `record_request` writes it. `Registry::call` fills the request's
`RequestAttribution` in `finish_call` (the first outermost capability, the first workspace a
handled call acted on, through `acted_workspace`); `record_request` adds a routed request to
`request_recorder.cpp`'s `RequestRecorder` without I/O, `ApiServer::drain` flushes it to each
workspace Zetta's `lake_requests` series every `request_series_flush_interval`, and
`ApiServer::stop` flushes it once more. `route_guard.hpp`'s `handler_exception` line no longer logs
the path. See [telemetry](../../docs/serving/telemetry.md#request-logs).

Usage over time (lake#564): `record_request` also adds each routed capability call to
`usage_recorder.cpp`'s `UsageRecorder` (`calls`, `api` or `mcp`), as `predict` adds a model's
answered predictions, `search_index` an index's answered searches, `create_transcript` and
`transcribe` the audio's seconds (`audio_duration.cpp` measures a recording's length when the
model reports none; tokens are never billed), and the file, drive, Hub and image paths their bytes as
`transfer` (`record_workspace_transfer`, `stream_blob`'s `metered_workspace`; lake#658);
`ApiServer::drain` flushes it every `usage_flush_interval` and, as the store root's owner, schedules
`usage_capabilities.cpp`'s `sample_usage` (stored bytes, the engine's `storage_usage` by kind, which
`usage_over_time` answers as the unbilled `engine_storage` (decision 61, lake#663),
instance and volume time, CPU, RAM, GPU and GPU memory from the operator's `workload_resources`
(lake#643), each finished build, job and cron run once, each closed hour of an app's ingress
traffic from the operator's `app_transfer` once as `transfer` (lake#661), new build results) every
`usage_sample_interval`. `usage_capabilities.cpp` registers `usage_over_time`, `usage_limits`,
`set_usage_limit` and `record_model_usage`; `metering_capabilities.cpp`'s `queue_repository_builds`
is the build-record queueing the sample shares with `metering_usage`. See
answered predictions and `create_transcript` the audio's seconds or tokens; `ApiServer::drain`
flushes it every `usage_flush_interval` and, as the store root's owner, schedules
`usage_capabilities.cpp`'s `sample_usage` (stored bytes, instance and volume time, CPU, RAM, GPU and
GPU memory from the operator's `workload_resources` (lake#643), new build results)
every `usage_sample_interval`. `usage_capabilities.cpp` registers `usage_over_time`, `usage_limits`,
`set_usage_limit`, `record_model_usage` and `model_completion`, the model gateway (lake#248): an
agent's model call, sent through `ApiContext::model_provider` on the blocking pool, its tokens added
to the run's workspace usage and refused past the monthly `model_tokens` limit. `model_gateway.cpp`
holds the gateway's pure parts (the request as forwarded, the usage of every answer shape, the
provider's refusals, `openai_model_provider`), and `http_server.cpp` serves the OpenAI-compatible
`POST /openai/v1/<endpoint>` (and the same under `/w/<workspace>/`, which must be the run's own workspace) over `model_completion` and builds the provider over its outbound client
when `LAKE_OPENAI_API_KEY` is set (`docs/api/model-gateway.md`). A streamed call is relayed as it
arrives (lake#665): the outbound client's `on_body` hands the provider's body on piece by piece, the
route sets `ApiContext::model_stream` to queue each piece, and its relay writes them to the caller in
chunked transfer while `model_completion` reads the usage from the events as they pass
(`ModelStreamUsage`). A caller that goes is sent nothing more, but the provider's stream is read to
its end and counted, and an answer without usage, or one that failed part way, is counted by
`estimated_model_tokens` (lake#939). `metering_capabilities.cpp`'s
`queue_repository_builds` is the build-record queueing the sample shares with `metering_usage`. See
[metering](../../docs/api/metering.md#usage-over-time).

Billing (lake#564, decision 56): `billing_capabilities.cpp` registers the ten billing capabilities and
the manual `PaymentProvider`; `ApiServer` calls `register_billing_capabilities` only where
`ServerOptions::billing` is on (`LAKE_BILLING`). A bill reads usage by calling `usage_over_time`
through the registry as the same caller. `console_capabilities.cpp`'s `viewer` answers `billing`,
and `list_docs`/`get_doc` leave out the Billing guide where billing is off. See
[billing](../../docs/api/billing.md). `stripe.cpp` is the Stripe provider (lake#620, decision 57):
form-encoded REST calls with an Idempotency-Key through the server's outbound client, and the
webhook signature check with OpenSSL's HMAC; the payment provider is chosen at start from
`LAKE_STRIPE_SECRET_KEY` and `LAKE_STRIPE_WEBHOOK_SECRET` (`environment_payment_providers`, lake#628:
lake's own keys are platform environment variables, never a user's secret store), and `http_server.cpp` serves
`POST /billing/stripe/webhook` through `receive_payment_notice`. The apex order sheet (lake#831,
[orders](../../docs/api/orders.md)): `orders.cpp` holds its configuration, checks, emails and rate
limit, `resend.cpp` the Resend client, `web_ui.cpp` its pages (`web_ui_order_*`,
`web_ui_price_list`, styles `.order-*` in `web_ui_styles.inc`), `stripe.cpp` the Checkout Session
and the `checkout.session.*` events, and `http_server.cpp` its routes on the apex host and the order
pass (email and audit) on the maintenance thread.

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

`engine_capabilities.cpp` registers every allow-listed workspace engine capability that no Lake
capability already covers as a named capability (lake#650): the engine's input schema from
`engine_openapi.json` (vendored by `tools/api/vendor-engine-openapi.py`, embedded by
`cmake/embed_engine_openapi.cmake`), Lake's name where the engine's collides or says too little,
and a handler that is `call_workspace_capability`, the `workspace_query`/`workspace_act` path, so
policy, agent scopes, audit and caching are the same. `granted_by_scope` lets a credential that may
call `workspace_query` or `workspace_act` call them. See
[`engine-capabilities.md`](../../docs/api/engine-capabilities.md). The standalone-model
capabilities (`train_model`, `retrain_model`, `register_model`, `move_model_to_project`, lake#654)
are named this way; `workspace_call` answers an engine that does not know an allow-listed name
`not_supported`, saying it needs a newer Zetta. `drives_capabilities.cpp`'s `load_drive_model`
reads a drive's model file as the caller and sends its bytes as `weights_base64` to `register_model`
(any format the engine converts, lake#681); `export_model` is a named engine read like `score`
([`models.md`](../../docs/api/models.md#a-standalone-model-lake654)).
[`engine-capabilities.md`](../../docs/api/engine-capabilities.md).
After the engine accepts a `create_agent` or `update_agent`, `workspace_call` keeps the definition
as a revision in the agent's workspace (`agent_revision_step`, lake#653) and answers
`definition_revision`; `list_agent_revisions` (in `workspace_capabilities.cpp`) reads them. See
[`train-agents.md`](../../docs/api/train-agents.md#definition-revisions).

`experiment_capabilities.cpp` registers eleven space-scoped experiment/run operations using the
closed contracts in `experiment_schemas.hpp`, and translates lake's experiment names to the engine's
study names and back (lake#561); the web app's Experiments pages call only them. The
Experiments Agent box uses `agent_turn`; a confirmed turn names the proposing task and the stored
proposal's digest, which `agent_turn` checks, and only the experiment may be edited. Questions and absent proposals get a reply form that reuses the task conversation;
Apps and Design pipeline questions use the same follow-up contract. Workspace action aliases retain
the same confirmation, scope and promotion checks. Their failures keep Zetta's `error.path`;
`start_experiment` requires `objective` and `budgets` only without `from_defaults` (lake#463).
See [API](../../docs/api/experiments.md) and [UI](../../docs/ui/experiments.md) contracts.

`stream_capabilities.cpp` registers the nine Streams REST/MCP operations, enforces space and
creator policy, pages one `list_topics` result per Zetta, filters shared bus topics and proxies
non-consuming browse through service credentials. The shared agent views use durable space tasks
without per-topic Zetta reads. The web app's Train → Streams pages call those same capabilities. Read `post_alias` routes preserve read classification, and MCP tools
carry read/destructive annotations. See `docs/api/streams.md` and `docs/ui/streams.md`.

`transcript_capabilities.cpp` registers speech-to-text (`transcribe`) and the five workspace
transcript capabilities (lake#561): transcripts are sealed `lake_transcripts` records in the
workspace Zetta, audio is sent through `ApiContext::transcriber` on the blocking pool and never
kept. `transcription.cpp` holds the pure parts: audio decoding, file names, `.vtt`/`.srt`
cleaning, and OpenAI's multipart request and answer. The HTTP exchange is the injected
`TranscriptPost`, which `http_server.cpp` builds over its outbound client when
`LAKE_OPENAI_API_KEY` is set. See `docs/api/transcripts.md`.

`mcp_client.cpp` is Lake's MCP client (decision 60, lake#652): initialize and sessions, the lists and
calls, notifications, the event-stream reader and every bound on a hostile server.
`http_server.cpp`'s `McpOutboundHttp` is its transport: an io thread of its own, `enough` to stop
reading once the answer is in, a byte limit, a deadline on resolution, connection, handshake and
exchange, and no address that is not public unless `LAKE_MCP_ALLOW_PRIVATE_ADDRESSES=1`. See
`docs/api/mcp-connections.md#the-client`.
`mcp_connection_capabilities.cpp` registers the MCP connection capabilities (lake#652); a `stdio`
connection (lake#659) writes the runner's secret and creates its App as the person, then reaches it
only at that App's cluster Service (`mcp_stdio_app_address`). It records
through `control::mcp_connections`, the credential read from the workspace's secret store as Lake
(`as_lake`, never answered), discovery merged into the person's tool decisions, who may call a
change (a person, or an agent with a person's per-tool grant and its `mcp:<prefix>` skill),
metering through the `UsageRecorder` (`calls`, `transfer`), the audit read back for a connection's
calls, and the re-export `mcp.cpp` calls for `/w/<workspace>/mcp`. `mcp_connection_streams.cpp` is
`McpStreamWorker`, run by the store root's owner on the maintenance thread over only the workspaces
whose dispatch row streams (lake#685; every connection write keeps the row and the digest of what
it re-exports, which `http_server.cpp` folds into a workspace endpoint's tool-list digest, lake#684):
notifications mirrored to
a connection's topic, one agent job per input event as the person who set the streams, and each
finished job's result on the output topic; `mcp_connection_internal.hpp` lends it the connection's
bearer, never a capability. `agent_stream_triggers.cpp` (lake#677) registers `set_agent_streams` and
`list_agent_streams` and is `AgentStreamWorker`, run on the same owner-only pass: one job of an agent
per new message on a workspace stream it names, as the person who set it, and the trigger's removal
after `delete_agent` (`remove_agent_stream_triggers`, called by `workspace_capabilities.cpp`, which
also withdraws the deleted agent's launches and schedules with `withdraw_agent_workloads`, and does
the same when a launch's or schedule's own `get_agent` answers `not_found`, lake#524). Both
workers start their jobs through `start_stream_runs` and act through `stream_person`
(`agent_stream_runs.hpp`).
`deletion_capabilities.cpp` registers what deletes a workspace's data (lake#645):
`delete_dataset`, `delete_model`, `delete_run` and `delete_index`, named capabilities over the
workspace engine's own deletes that answer `not_supported` from an older engine, and
`delete_ask_conversation`, which removes one person's message records from the workspace Zetta and
the agent tasks the conversation started (their `lake_agent_tasks` records, then their dispatch
rows through `Store::delete_agent_tasks`, lake#673).
`delete_account` (lake#810) sits beside `delete_organization` in `capabilities.cpp`: it checks the
caller (the account after a recent sign-in or MFA, or the operator), then calls `delete_workspace`
and `delete_organization` through the registry as the same caller before `Store::delete_account`
([identity.md](../../docs/api/identity.md#deleting-an-account)).
`delete_organization`, `revoke_invitation` and `revoke_grant` sit beside their siblings in
`capabilities.cpp`, and `delete_secret_mount` in `secrets_capabilities.cpp` over
`MountSecrets::remove_all` (`secret_records_api.cpp`). Each takes `dry_run`. See
`docs/api/deletion.md`.

`solution_capabilities.cpp` registers the six solution capabilities (lake#597): a workspace's own
solutions are sealed `lake_solutions` records in the workspace Zetta, listed and read beside Orama's
catalogue, which it reads through the workspace's engine (`list_solutions`, `get_solution`);
`update_solution` and `delete_solution` answer `forbidden` for Orama's. See `docs/api/solutions.md`.

`methodology_capabilities.cpp` registers a workspace's own methodologies (lake#655): the code is
committed to a workspace repository through the repository store, registered by the engine
(`register_methodology`, `update_methodology`, `delete_methodology`), built by `build_image` and run
as `methodology_runs` rows the Orama operator runs; each run's record is sealed
`lake_methodology_runs` in the workspace Zetta. `methodology_run_input` and `methodology_run_output`
answer only a run's `lmr.` credential, which `Registry::call` checks
(`authorize_methodology_run_token`) in place of policy. `workspace_capabilities.cpp` dispatches an
engine `run` that answers `runner_required` to `run_methodology`. See `docs/api/methodologies.md`.
An artifact over 24 MiB comes in with `methodology_run_artifact` (forwarded in order to the engine's
`append_methodology_artifact`) before the output names its size and SHA-256 (lake#690). A model such
a run trained is served by its own code (lake#690): publishing it (`workspace_act expose_model`)
writes a `methodology_deployments` row the operator runs, unpublishing or `delete_model` withdraws it
(`serve_methodology_model`, declared in `methodology_serving.hpp`), `predict` of it is forwarded to
its serve Service through the injected `MethodologyServer` (`predict_methodology_model`), and
`methodology_deployment_input` answers only the deployment's `lmd.` credential
(`authorize_methodology_deployment_token`).
engine `run` that answers `runner_required` to `run_methodology`. `draft_methodology {task_id}`
(lake#688) reads the draft Ask Orama wrote in an `agent_turn` of kind `methodology_draft` (set up
in `workspace_capabilities.cpp`) and offers it only when `check_code` passes, otherwise Lake's
template with the reason. See `docs/api/methodologies.md`.
engine `run` that answers `runner_required` to `run_methodology`. An experiment over a workspace
methodology's parameters (lake#689) runs each trial the engine admits as a training run of it:
`methodology_experiments.hpp` (`current_experiment`, `stop_experiment_runs`) is what
`experiment_capabilities.cpp` calls on start, read and stop; a trial's run is claimed once in the
sealed `lake_methodology_trials` record, and `methodology_run_output` reports its result with the
engine's `study` and `trial`. See `docs/api/methodologies.md#experiments`.

Repository terminal instructions receive a validated explicit `ServerOptions::external_origin` or
the HTTPS fallback derived from trusted `public_host`. Request and proxy headers cannot select
their URL; the web app shows the commands. Existing registry sync operations implement clone and
push. See [forge.md](../../docs/ui/forge.md) and the native
[private-install proof](../../docs/deployment/private-install.md).

Train → Runs, a run page and Ship → Models also read the engine's `list_studies` (lake#449): a
trial run is *Trial N of experiment <name>*, an experiment's model names its experiment, and a
promoted model is marked. A run
report leads with its held-out score and row count from `get_run_report.model`, with Zetta's
caveat on that figure beside it.

Every data-plane capability requires `workspace_id` (lake#439); the web app's copy-ready calls
name the workspace in its URL. `markdown.cpp` joins a list item's continuation lines before inline markup, so a Docs code span may
wrap; the binary no longer renders Markdown (the web app does), and `tests/test_examples.cpp` uses it
to find and validate every Docs and API document example against the registry.

OCI blob upload patches and final puts stream request buffers into the upload file. The adapter
checks the configured completed-blob limit and `Content-Range`, hashes the completed file without
loading it into memory, and leaves the common 64 MiB body limit on every other route. See
[the HTTP contract](../../docs/serving/http.md) and [image builds](../../docs/api/builds.md).

The single capability registry defines REST, OpenAPI and MCP behavior. `create_token` validates
issuer scopes, lifetime and static authority in `capabilities.cpp`. The common HTTP context
carries authenticated session metadata; adapters do not decide delegation policy. Application
sign-in results may include a JWT; API-token issuance retains only its derived session bearer.

`http_server.cpp`'s `ApiServer::start` creates or loads liquid's local identity once, under a file
lock, before any thread opens a repository: liquid creates it without a lock, and two first uses
at once break it for the process ([store volume](../../docs/deployment/store-volume.md#liquids-local-identity)).

`http_connections.hpp` (lake#534) accepts and serves `http_server.cpp`'s connections: one session
coroutine per connection up to `LAKE_HTTP_MAX_CONNECTIONS` (503 above it), a request worker only
while a route runs (taken after any store-root wait, given back by the capabilities declared
`Capability::long_poll` through `ApiContext::leave_worker` and by streaming pulls, within the
long-request caps), idle, header and body/response I/O timeouts, a minimum transfer rate while a
worker is held, lingering closes, and a sink that closes every 503 and 429's connection and any
whose unread body is too large to discard. `registry.cpp`'s blocking pool refuses (`busy`) past 256
queued tasks. Like `route_guard.hpp` it names Beast2 and Boost.HTTP types and stays in `src/api/`. See
[connections and workers](../../docs/serving/http.md#connections-and-workers).

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
The web app's sign-up calls `sign_up_password`, which sets the browser session, and creates a
repository with `create_repository`. See `docs/api/provisioning.md` and `docs/ui/identity.md`.

The web app builds its navigation from the `viewer` read (`console_capabilities.cpp`); a protected
download without a credential redirects to `/sign-in`. OpenAPI describes account tokens. The
behavior and verification are in [front-door.md](../../docs/ui/front-door.md).

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

Application Agent app turns are assembled in `workspace_capabilities.cpp`: read tools on the first
turn, repository/App writes only when the confirmed turn names the proposing task and the stored
proposal's digest and carries that exact proposal (`verified_confirmed_proposal`).
Experiments Agent turns use the workspace adapter and grant the engine's raw study operations by turn kind;
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
in `capabilities.cpp`; the web app's Organizations pages call them. See `docs/api/organizations.md` and `docs/ui/settings.md`.
`workspace_capabilities.cpp` scopes launches and schedules to the calling workspace, reports a
waiting launch's reason through `launch_json`, and checks a `create_agent` or `update_agent`
definition before it reaches Zetta, which would drop unknown fields such as `content`. Zetta's
`invalid_argument` is reported as `invalid_input`. A run lake timed out is Failed, with its
reason (lake#442). See `docs/api/agent-launches.md`.
The store capabilities (`list_stores`, `create_store`, `delete_store`, `store_connection`) take
the workspace and see only its stores (lake#465). See `docs/api/storage.md`.

`registry.cpp` runs the synchronous handlers of capabilities that wait on a repository store
(repository, Drive, model and dataset objects, search, `replicate_repository`,
`move_workspace_store`, `remove_space_store`, `delete_workspace`) on a 32-thread pool, and `run_blocking` /
`blocking_call` (`registry.hpp`, `capabilities.hpp`) run asynchronous handlers' store calls there,
so a slow workspace Zetta holds up only its own requests (lake#380). `LiquidCustomerStores` admits
at most four requests per store lane and refuses a moving lane with 503; it lends repositories to
search (`lend_repository`), and a switched workspace's search index objects with them or alone
(`lend_workspace`), under the workspace store's lane, sealed with the store's data key and fenced
by its lease (`search_objects_of`, lake#381), with the room the bucket quota leaves for them
(`search_index_budget`); `lend_repositories_to_search` wires them into `search::WorkspaceAccess`,
with release tags read from forge records and rebuilds queued on search's own worker
(`schedule_search_job`, beside the maintenance thread). The
`search` capability resolves the caller's scope first (`search_scope`, `capabilities.cpp`);
`delete_repository` and `commit_repository_files` keep the index in step. The
server runs the usage refresh, the start-up usage priming, the start-up purge of switched
workspaces from the space search indexes (`purge_space_search_indexes`) and the deletion pass on a
maintenance thread, and closes its io_context to
the blocking pool at shutdown (`close_execution_context`).
`create_repository_row` (`capabilities.cpp`) creates every repository row through
`SyncStore::create_repository_content`. `workspace_store_capabilities.cpp` registers the operator-only `move_workspace_store` (plan, copy,
verify, switch; lake#380, `docs/api/workspace-store.md`) and `remove_space_store`, which removes a
space's old store (`<root>/<space>/store` and `<space>/repositories`, nothing else) under its lane
once every workspace of the space is switched, with a dry run that reports paths and bytes. `http_server.cpp`'s `LiquidCustomerStores`
verify, switch; lake#380, `docs/api/workspace-store.md`); a switch then drops the switched
repositories from the space's search index (`search::Service::drop_switched`, lake#381). `http_server.cpp`'s `LiquidCustomerStores`
routes a repository to its workspace's store once `workspaces.store_moved_at` is set (failing
closed), holds one lane per store (`s:<space>`, `w:<workspace>`) with its map lock never held across
I/O, opens each workspace store ahead of its first request (`preopen_workspace`, lake#536) or on
first use, never creates a workspace store, key or repository
except explicitly (`create_repository_content`, the switch), closes and reopens a store whose lease
was lost or whose write failed, closes them on drain (`release_workspace_stores`, each as soon as its
lane is idle, all at once) and on workspace
I/O, opens each workspace store on first use, never creates a workspace store, key or repository
except explicitly (`create_repository_content`, the switch), makes what a call wrote to a workspace
store durable before it answers (`StoreDurability`, one Zetta `:barrier` per call, lake#535),
closes and reopens a store whose lease was lost or whose write failed, closes them on drain and on workspace
deletion (`WorkspaceContent::close_workspace_store`), measures a switched workspace's storage as
its bucket's usage, and keeps replication cursors with liquid's cursor API.

`workspace_records_api.hpp` opens one workspace's records through `call_workspace`, so record
calls are owner-checked, refused when the workspace is not ready and named in Server-Timing.
`workspace_record_capabilities.cpp` registers the operator-only `copy_workspace_records` (plan,
copy, verify). It accepts only `audit_events` and `oci_tags` now: `ask_messages` left the list when
migration `0050` dropped its table (lake#483), and every other collection this page says it copies
(`opportunities`, `stream_owners`, `drive_share_links`, the application tables, metering, forge
records, secrets, leases, the platform tables, application settings, authorization codes, agent
task content and repository descriptions) left it, with its copier and the store's legacy reads,
when migration `0070_drop_moved_tables` dropped its source (lake#311). Ask Orama and the Agent Factory
conversations are written only by `send_ask_message`, `settle_ask_turn`, `confirm_ask_proposal` and
`approve_agent_proposal` in `workspace_capabilities.cpp` (lake#561); the web app calls them, and
`get_ask_conversation` reads, in the input's workspace.
`console_capabilities.cpp` registers the reads the web app needs that the pages computed for
themselves (`viewer`, `my_permissions`, `list_packages`, `list_docs`, `get_doc`, `mcp_setup`,
docs/api/web-app.md); `latest_agent_task`, `compile_pipeline` and `render_pipeline` sit beside the
conversation capabilities, and `resolve_model` with `find_hub_repository` in
`model_capabilities.cpp`. `settle_discover`, `list_opportunities`, `dismiss_opportunity` and
`create_projects_from_opportunities` keep Design → Opportunities in the workspace the scan ran in
(lake#376); the Opportunities page reads them concurrently with the methodology registry. The record store capabilities stay out of
the `workspace_query`/`workspace_act` allow-lists, so only lake reaches its `lake_*` collections
(lake#370, `docs/architecture/workspace-records.md`). `stream_capabilities.cpp` reads and claims
topic creators in the topic's workspace (`lake_stream_owners`, lake#379); the retired
`stream_owners` table and its copy went with migration `0070`.
(lake#370, `docs/architecture/workspace-records.md`). Drive share links (`drive_capabilities.cpp`,
lake#350) live in the workspace named by `workspace_id` on create, list (`drive_effective_access`)
and revoke, and by the bearer token on resolve, preview and download; the web app's sharing pages
carry the workspace in their URLs. The retired `drive_share_links` table and its copy went with
migration `0070`.
`drive_history` (`drive_capabilities.cpp`, `drive_path_change`) answers each change's state of the
path, so an upload over an existing file (removal plus addition of the path in one change) reads as
`modified` with the replaced blob, never as a deletion (lake#123, `docs/api/drive.md#path-history`).
(lake#370, `docs/architecture/workspace-records.md`). `app_capabilities.cpp` records application
lifecycle events and delivery attempts in the application's workspace (lake#356, lake#357):
`register_application` requires `workspace_id`, the synchronous identity callers record an event
through `liquid::runtime::block_on`, and `list_application_webhook_events` and
`retry_application_webhook` are asynchronous and take the application's `workspace_id`.
The retired event and delivery tables, and their copy, went with migration `0070`.
An application's users and organizations live in the same workspace (lake#353, lake#354):
`app_capabilities.cpp` reads and writes them there, the user and organization capabilities take the
application's `workspace_id`, and `application_session_result` checks every session issued in an
application's pool against the user's record before returning it, revoking it otherwise.
`sign_up_password_session` claims a pool user's email before the account exists. These, the
identity capabilities in `capabilities.cpp` (sign-up, sign-in, TOTP, passkey, SSO, MFA, tokens) and
recovery in `offline_recovery_capabilities.cpp` are asynchronous, with no `block_on`; the hosted
pages call them through `call_async`. The retired `auth_app_users` and `auth_app_organizations`
tables, and their copy, went with migration `0070`.
Repositories belong to one workspace (lake#315, part 1). `create_repository`,
`import_repository` (`export_capabilities.cpp`) and `import_github_repository` require
`workspace_id`, checked by `repository_workspace` (declared in `capabilities.hpp`);
`list_repositories` takes an optional `workspace_id`; `transfer_repository` is refused. The push
path passes the repository itself to `sync_change_authorizer` and `evaluate_branch_protection`
(`forge.hpp`), so each decision and its audit record carry the workspace. `delete_workspace`
cascades to the workspace's repositories, apps, unfinished builds and tenant images (lake#514):
`workspace_deletion.cpp` computes the dry run (`workspace_deletion_plan`) and the workspace's image
repositories, `Store::delete_workspace_cascade` is the one transaction, and
`finish_workspace_deletion` removes what lives outside PostgreSQL through the injected
`WorkspaceContent` (the instance registry's tags, the space index's search segments, both gated on
owning the store root in `http_server.cpp`, and the workspace's own search index, lake#381) and records one central audit event per destroyed object, once
each. `ApiServer::drain` resumes unfinished deletions every minute. See
[workspaces.md](../../docs/api/workspaces.md#deletion). `create_repository` and the hub
`/api/repos/create` (body `workspace_id` or `X-Lake-Workspace-Id`) take the workspace. See `docs/api/repositories.md`.
Forge records live in the workspace of their repository's workspace (lake#341):
`forge_records_api.hpp` declares `RepositoryForge` (create, list, get and conditional update of one
repository's records; a repository without a workspace is `conflict` at `/repository_id`), and every
forge capability, `export_repository`, `import_repository` and `import_github_repository` is
asynchronous over it. The `push` capability is asynchronous too: it reads the rules, and the
reviews and releases they ask for, once with `load_branch_protection` (`forge.hpp`) and hands that
snapshot to `sync_change_authorizer`, because the push runs synchronously inside the repository
store; a snapshot that could not be read refuses every reference update. `serve_sync_post` calls
the registry asynchronously. The retired `forge_records` table and its copy went
with migration `0070`.
`app_capabilities.cpp` keeps an application's signing keys and webhooks in its workspace (lake#352,
lake#355): `register_application` is asynchronous and writes the first key ring there before the
application row, `rotate_application_signing_key`, `create_application_webhook` and
`list_application_webhooks` are asynchronous and take the application's `workspace_id`,
`application_jwks` is asynchronous and finds the workspace from the application id alone, and the
token issuers read the current key with `application_signing_key` on their asynchronous path, with
no `block_on`. The raw JWKS route calls the capability asynchronously. The retired `auth_app_signing_keys` and
`auth_app_webhooks` tables, and their copy, went with migration `0070`.
Secrets live in the workspace of their mount (lake#328-#332): `secret_records_api.hpp` declares
`MountSecrets` (one mount's records, opened through the mount's route: put, read, metadata, version
changes, named keys and transit), `create_secret_mount`, `list_secret_mounts` and
`rotate_customer_kek`, which rewraps every data key of the space's workspaces before forgetting the
replaced KEK. Every version and key-version number is claimed `if_absent` and every transition
written at the revision read. `secrets_capabilities.cpp` is asynchronous over it; the policy
decision reads only PostgreSQL, so a refused caller reaches no workspace. The five retired secrets
tables, and their copy, went with migration `0070`.
Dynamic leases follow (lake#362): `MountSecrets::lease`, `renew_lease` and `revoke_lease` keep each
lease as a record of its mount's workspace (route first, then the record `if_absent`; renewal and
revocation rewrite it at the revision read), `MountSecrets::open_lease` finds the mount through the
lease's route, and `expire_secret_leases` is one pass of the expiry loop: it claims due routes from
the control store and settles each lease in its own workspace only. The server's tick runs it every
`secret_lease_expiry_interval`. The retired `secrets_dynamic_leases` table and its copy went with
migration `0070`.
`repository_records_api.cpp` joins a repository's routing row with its settings record (lake#315
part 2): `create_repository_with_settings` (the record first, then the row; a refused row removes
the record), `fill_repository_settings` (one `get_record`, or one `list_records` per workspace,
all started together) and `repository_json` (`description_unavailable` when it could not be read).
`capabilities.cpp`'s `get_repository`, `list_repositories` and `create_repository` gain
asynchronous handlers that use them; their synchronous handlers answer the routing row and refuse a
description. `refs`, `push` and `replicate_repository` no longer copy refs into the control store
(lake#319); `refs` answers `{repository_id, name, head}` from liquid. The imports in
`export_capabilities.cpp` and `github_import_capabilities.cpp` write their description to the
workspace; the retired description column and its copy went with migration `0070`.
`app_capabilities.cpp` keeps an application's settings in its workspace (lake#351):
`register_application` writes them there with the first key ring, `get_application` and
`list_applications` are asynchronous and fill them from the workspace (`configured_application`,
one `list_records` per workspace for a listing), and `update_application_theme` takes the
application's `workspace_id` and writes conditionally. `capabilities.cpp` reads the allowed origins
from there on the asynchronous path (`begin_passkey_registration`, `begin_passkey_sign_in` and
`start_oauth` became asynchronous). `http_server.cpp` reads them for the hosted and custom-domain
identity pages and their forms' redirects. The retired settings columns and their copy went with migration
`0070`.
Environments and the platform mirror live in the workspace of their repository (lake#358,
lake#359): every platform capability in `platform_capabilities.cpp` is asynchronous and opens the
repository's own workspace (a repository without one is `conflict` at `/repository_id`). An
environment's policy object is found in PostgreSQL from the repository and the name, so a refused
caller reaches no workspace, and `list_environments`/`list_deployments` answer an empty list
without a workspace call to a caller who may read no environment of the repository. The
owner loop hands `PlatformService::sync` an opener over the workspace caller.
The retired platform tables and their copy went with migration `0070`.
`app_capabilities.cpp` issues and redeems application authorization codes in the application's
workspace (lake#363): `begin_application_authorization` writes the code there, and
`exchange_application_authorization_code` reads the routing row for the workspace before it redeems
the code. The retired `auth_app_authorization_codes` table and its copy went with migration
`0070`.
`sealed_delivery.cpp` (lake#625, decision 58) is `mint_sealed_secret`, the operator's delivery of a
user's secret to the Kubernetes Secret an app, service or deployment mounts: it reads the secret
(or leases a dynamic one) through the registry as the operator and seals each field to the
cluster's X25519 key in the orama-operator's envelope version 1 (X25519, HKDF-SHA-256, HChaCha20
written here over OpenSSL's ChaCha20-Poly1305, canonical CBOR). It also parses and writes the
`lake-secret://<mount>/<name>#<field>` references `web_app_capabilities.cpp` stores for an app's
secret environment, after checking them with the caller's own reads. See
[secrets](../../docs/api/secrets.md#projections).

`image_capabilities.cpp` registers `create_image_repository` (an explicit `workspace_id`, no
default), `list_image_repositories` (the routes with each workspace's tags) and the operator-only
`unroute_image_repository` (central audit), and holds the legacy owner rule (`legacy_image_owner`,
which writes a route only for the copy, never for plan or verify) and the image byte count metering reads from tags
(lake#383 phase A). `http_server.cpp` routes every tenant OCI request (`<space>/...`) through
`oci_repository_routes` before reading a workspace: a build's first push writes the route from its
own target from its workspace, a workload credential reaches only its own workspace, an upload
records its blob in the workspace (`lake_oci_blobs`) and a manifest may reference only recorded
blobs, routes that cannot be read answer 503, manifests and tags are read and
written as workspace records, and Ship → Packages lists the selected space's images through
`list_image_repositories`. `copy_workspace_records` gains `oci_tags`, reading the instance
registry's tenant pointers through `WorkspaceContent`. Since phase B (decision 54 S5) an upload
completed into a switched workspace is checked and streamed into its liquid store
(`finish_tenant_upload`, `LiquidCustomerStores::put_workspace_blob`, off the server thread) and
recorded with its object; a pull streams it back 4 MiB at a time (`stream_workspace_blob`,
`read_workspace_blob`); a record without an object is still read from the instance registry; a
manifest takes its blob sizes from the records; a switch creates the store of a workspace with no
repository; and `copy_workspace_records` gains `oci_blobs` (`workspace_record_capabilities.cpp`),
which streams each recorded blob from the instance registry into its workspace store and verifies
it by SHA-256. `move_workspace_store` gains mode `provision` (`workspace_store_capabilities.cpp`,
now an asynchronous handler that runs the other modes on the blocking pool), which creates the
store of a switched workspace that never held anything (`provision_workspace_store`). A
completion claims the staged upload (`ArtifactRegistry::claim_upload`) before hashing it; a pull
holds one of its workspace's eight stream slots for its whole stream (`admit_workspace_stream`,
`stream_blob`, at most 24 streams, ended after `registry_stream_idle_timeout` without progress,
refusals sent with `Connection: close`); image blobs are rooted with a liquid lease once per
record (`root_workspace_blob` for the copy); the owner loop sweeps abandoned uploads
(`sweep_uploads`, `ServerOptions::registry_upload_max_age`).

`volume_capabilities.cpp` registers the operator's volumes (migration `0069`, operator#259,
operator#260): `set_workspace_storage` (grow only; a smaller size is `invalid_input` at
`/storage_gi`), `request_volume_snapshot` (stamps `requested_at`, optionally a new schedule and
keep) and `volume_snapshots`. Only the operator credential and the instance administrator reach
their handlers; their audit events are central. A store failure is `store_unavailable`, here and in
the registry's own `workspace_id` check. Workspace rows carry `storage_reason` for every reader and
`storage_message` only for the operator and the instance administrator (`operator_view`). See [volumes.md](../../docs/api/volumes.md).

`store_repository.cpp` is a `lake` store's contents over a liquid repository's `main` branch
(lake#664): reads through the repository store and one change per write, folder, move and removal,
each call on the blocking pool. `drives_capabilities.cpp`'s `open_store` opens every store kind:
`zetta` and `lake` by Lake, the customer's own bucket by the server's `StoreBrowser` with the fields
of the workspace secret the store names (read as Lake, kept a minute). `http_server.cpp` builds the
default browser, `control::cloud_store_browser` over its own public-only transport
(`LAKE_STORE_ALLOW_PRIVATE_ADDRESSES` for local development). See
[storage.md](../../docs/api/storage.md#browsing-a-store).

SAML sign-in (lake#836, `docs/api/sso.md#saml-sign-in`): `saml.cpp` holds the steps the
`start_sso_saml` and `sign_in_saml` capabilities and the `/v1/saml/<provider>/metadata`, `/start`
and `/acs` routes in `http_server.cpp` share: starting a request, and verifying a response, consuming
its request and recording its assertion once before `sign_in_sso` provisions the person.
OpenID Connect discovery (lake#837, `docs/api/identity.md#openid-connect-discovery`): `oauth.cpp`
holds `OidcDiscovery`, which reads and caches each issuer's `/.well-known/openid-configuration` over
a fetch `http_server.cpp` injects (the outbound client, public addresses only unless
`LAKE_OIDC_ALLOW_PRIVATE_ADDRESSES`). `ApiContext::oidc_discovery` carries it to `start_oauth` and
`start_sso_oidc`, which read it on the blocking pool, and the OAuth callback uses it directly.

`mcp_client.cpp` supplies `mcp_parse_json`, shared with `mcp_connection_capabilities.cpp` for
OAuth responses. It aborts before constructing a container at depth 65; every remote JSON reader uses
it. Tool conversion checks depth iteratively before its schema copies. See
[MCP connection limits](../../docs/api/mcp-connections.md#the-client) (lake#958, LSR-092).
Folder live grants and bearer links share `require_drive_share_role` in `drive_capabilities.cpp`
(lake#955): every delegated action must be held by the caller; non-admin identity recipients must
belong to the account. See [the contract](../../docs/api/sharing.md).
