# The `/v1` API and MCP

lake#650: [engine-capabilities.md](engine-capabilities.md) documents every capability of a
workspace's engine as a named Lake capability (its own route, OpenAPI operation and MCP tool, with
the engine's input schema vendored in `src/api/engine_openapi.json`), the names Lake gives the
engine's where they would collide or say too little (`check_pipeline`, `start_run`,
`search_workspace_index`, `get_holdout_experiment`, ...), the Lake capabilities that already
covered the rest, and the `enum` `workspace_query` and `workspace_act` now carry on `capability`.
lake#248: [model-gateway.md](model-gateway.md) documents the model gateway, `model_completion` and
its OpenAI-compatible routes `POST /openai/v1/chat/completions`, `/responses` and `/embeddings`:
every agent model call goes through Lake with Orama's own key and is counted as the run's workspace's
`model_tokens`, the monthly `model_tokens` limit is enforced (`usage_limit_reached`, 429), and agents
hold no provider key. A `stream: true` call is relayed event by event as the provider sends it
(lake#665, [Streaming](model-gateway.md#streaming)).

lake#564 (decision 56, migration `0073_billing`): [billing.md](billing.md) documents billing, on only
where `LAKE_BILLING` is (Orama Cloud): `price_list`, `set_price_list`, `billing_account`,
`bill_preview`, `issue_invoice`, `list_invoices`, `get_invoice`, `record_invoice_payment` and
`void_invoice`, the `PaymentProvider` interface with its manual
implementation, and the switch that leaves them absent on an enterprise install. lake#620
(decision 57, migration `0074_payment_provider`): Stripe is the payment provider, with its keys in
lake's own secret store and its webhook at `POST /billing/stripe/webhook`
([billing.md#stripe](billing.md#stripe)).
lake#623, lake#635, lake#637: [billing.md](billing.md#price-list) carries Orama Cloud's price list,
usage only (one rate per dimension, no plans, no fee, nothing included; Metis is priced by sales),
time per minute, each rate with its reference rate, date and margin, worked bills, how a change of
rates mid-month is billed, and when to revisit it.
lake#654: [models.md](models.md#a-standalone-model-lake654) documents a standalone model, made on its
own rather than by a project's pipeline: `train_model`, `retrain_model`, `register_model` and
`move_model_to_project` (the workspace engine's, [engine-capabilities.md](engine-capabilities.md)),
and Lake's `load_drive_model`, which brings a drive's model file.
lake#645: [deletion.md](deletion.md) documents deleting and revoking what could only be created
before, each with `dry_run: true` first and a typed confirmation in Lake Console:
`delete_organization`, `revoke_invitation`, `revoke_grant`, `delete_secret_mount`, `delete_dataset`,
`delete_model`, `delete_run`, `delete_index` and `delete_ask_conversation`.
lake#561: [drives.md](drives.md) documents drives (repository, workspace files, virtual) and workspace files in the workspace Zetta. [transcripts.md](transcripts.md) documents speech-to-text (`transcribe`) and the
workspace transcripts kept in the workspace Zetta (`create_transcript`, `list_transcripts`,
`get_transcript`, `update_transcript`, `delete_transcript`), the audio never kept.
lake#652 (decision 60): [mcp-connections.md](mcp-connections.md) documents a workspace as the client
of remote MCP servers: Lake's own client and its bounds, the connection record and its secret, the
fifteen capabilities (`create_mcp_connection`, `list_mcp_connections`, `get_mcp_connection`,
`update_mcp_connection`, `delete_mcp_connection`, `refresh_mcp_connection`, `test_mcp_connection`,
`list_mcp_connection_tools`, `read_mcp_tool`, `call_mcp_tool`, `set_mcp_tool_auto_approve`,
`list_mcp_connection_calls`, `begin_mcp_connection_sign_in`, `complete_mcp_connection_sign_in`,
`create_mcp_connection_agent`), the read-or-change rule, who may call what, OAuth sign-in, the
opt-in re-export through `/w/<workspace>/mcp` (whose sessions are told when what it re-exports
changes, lake#684), a connection's agent and streams (the stream worker calls only workspaces that
stream, lake#685), and servers that speak only stdio, run as an App of the workspace by Lake's own
runner `oramasearch/lake-mcp-runner` (lake#659).
lake#597: [solutions.md](solutions.md) documents a workspace's own AI solutions, kept in the workspace
Zetta beside Orama's read-only catalogue (`list_solutions`, `get_solution`, `create_solution`,
`update_solution`, `delete_solution`, `duplicate_solution`; `forbidden` for Orama's).
lake#655: [methodologies.md](methodologies.md) documents a workspace's own methodologies from Python
code in a workspace repository beside Orama's read-only ones: the interface, Validate, runs as
untrusted code in the Orama operator's sandbox with the runner's capabilities, metering, the
pipeline step, and (lake#690) artifacts over 24 MiB in chunks and serving a trained model from Ship →
Models.
untrusted code in the Orama operator's sandbox with the runner's two capabilities, metering, and the
pipeline step; and (lake#688) drafts written by Ask Orama in a `methodology_draft` turn, checked by
`draft_methodology {task_id}` before they are offered.
[web-app.md](web-app.md) documents the capabilities the web app reads that the pages
computed themselves (`viewer`, `my_permissions`, `list_packages`, `list_docs`, `get_doc`,
`mcp_setup`, `resolve_model`, `latest_agent_task`, `compile_pipeline`, `render_pipeline`);
[ask-orama.md](ask-orama.md#capabilities-lake561) the conversation capabilities
(`send_ask_message`, `settle_ask_turn`, `confirm_ask_proposal`, `approve_agent_proposal`);
[repositories.md](repositories.md) `repository_file`'s `units`; and
[../serving/http.md](../serving/http.md#cross-site-requests) the refusal of cross-site requests that
use the browser session.

lake#452: [services.md](services.md) documents `list_services`' shared `status` and
`status_reason`; [web-apps.md](web-apps.md) the public acknowledgement `create_app` requires;
[workspaces.md](workspaces.md) the two `create_project` name refusals.

lake#380: [workspace-store.md](workspace-store.md) documents the operator's `move_workspace_store`
(plan, copy, verify, switch) and `remove_space_store` (the account's old store, once every workspace
is switched); [sync.md](sync.md) and [replication.md](replication.md) where a
switched workspace's repositories and replication cursors live; [metering.md](metering.md) how its
storage is measured.

lake#514: [workspaces.md](workspaces.md#deletion) documents the cascading `delete_workspace`, its
`dry_run` and its result; [repositories.md](repositories.md#the-owning-workspace),
[registry.md](registry.md) and [forge.md](forge.md) what goes with a deleted workspace.

Migration `0069` (operator#259, operator#260): [volumes.md](volumes.md) documents
`set_workspace_storage`, `request_volume_snapshot` and `volume_snapshots`, the operator's and the
instance administrator's desired state for each workspace Zetta's volume size and the scheduled CSI
snapshots of those volumes and of lake's store volume; [workspaces.md](workspaces.md#capabilities)
the storage fields a workspace row gains.

[experiments.md](experiments.md) documents eleven training experiment, trial, run-metric,
comparison and bounded-live-read capabilities, with identical REST POST/MCP schemas and confirmed
actions, plus the `agent_turn` contract used for ExperimentsAgent proposals. Lake says experiment
and `experiment_id` where the engine says study (lake#561, [names](experiments.md#names)); lake's
`get_experiment` is distinct from the solutions holdout `get_experiment` that `workspace_query`
reads. `start_experiment` leaves a missing objective and budgets to Zetta under `from_defaults`,
and every experiment error keeps Zetta's `path`.

[streams.md](streams.md) documents all nine topic/message operations on both REST POST and MCP,
with closed input/output schemas, account permissions, system-topic refusal and bounded live reads.
Every MCP tool carries all three annotations from one rule in `src/api/registry.cpp` (lake#594):
`readOnlyHint` for a read or a call authorized as one (`predict`), the same rule `mcp_setup` uses
for `writes`; `destructiveHint` only on a write that deletes, destroys, removes, revokes or drops
(`delete_*`, `destroy_*`, `remove_*`, `revoke_*`, `drop_*`, `purge_*`, …), so a client warns before
those alone; and `idempotentHint` on reads and on writes a second identical call leaves as they
were (`set_*`, `update_*`, the deletes, `settle_ask_turn`, …). Every operation has a human title,
OpenAPI's `summary` and the tool's `title` (`Mcp.EveryToolHasAHumanTitleAndOnlyDeletesAreDestructive`).
Streams reads retain GET and add POST without changing their read classification.

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
a kind (`read` → `GET /v1/<name>`, input in the query string, each value coerced by its declared
type ([arrays in a query string](#arrays-in-a-query-string)); `action` → `POST /v1/<name>`,
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
A message, from the API layer or from the stores, transparency, audit and search code beneath it,
calls the product **Lake** and never says `space`: it says the account, the workspace or the
organization, whichever is true (decision 59, lake#647). Field names (`customer_id`), key-derivation
and AEAD labels (`lake data key/…`), attestation types, pool ids and the `lake:v1` prefix keep
their form. These pages follow the same rule in their own prose (lake#669): an account is you or
an organization, and its id is `customer_id`. Capability, parameter and field names keep their
form in code spans (`remove_space_store`, `convert_personal_space`, the `spaces` field of `viewer`).
`Docs.DeveloperDocsNeverSaySpace` fails on `space` or `spaces` as a word in the prose of any
`docs/api/*.md` or `docs/guide/*.md` file.

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
environment/domain mutations, and Loki-compatible log reads. Environments and the mirrored rows
live in the workspace Zetta of the repository's workspace (lake#358, lake#359): a repository
without a workspace is `conflict` at `/repository_id`, and an environment's policy object is found
in PostgreSQL, so a refused caller reaches no workspace
([platform.md](platform.md#where-environments-live-lake358-lake359)).

[`audit.md`](audit.md) defines the signed audit stream. Since decision 52 (lake#317, lake#318)
it is two kinds of log: the central chain in PostgreSQL for control-plane and identity events, and
each workspace's own chain, in its Zetta, for events about its data, written off the request path;
the audit capabilities read both ([audit.md](audit.md#which-log)).

[`registry.md`](registry.md) defines the OCI Distribution, npm, Python, and debuginfod protocol
routes, build-push credentials, exact-repository app pull credentials, and exact-byte integration
coverage.
OCI blob `PATCH` and final `PUT` requests are the only routes admitted above the general 64 MiB
request-body limit. They stream to disk up to `LAKE_MAX_REGISTRY_BLOB_BYTES`, validate chunk
ranges, and verify the digest from the completed file; [`builds.md`](builds.md) and
[`../serving/http.md`](../serving/http.md) own the operational contract.

[`services.md`](services.md) defines a customer's managed services, and [`storage.md`](storage.md)
a customer's stores (buckets, filesystems, network filesystems and volumes; a cluster volume in
phase 1, lake#196), with their capabilities and the Data → Drives → Storage tab (lake#656), and store drives that browse a bucket or filesystem in place:
the customer's own bucket over Lake's own S3 and Azure Blob clients with a workspace secret, and
stores Lake keeps in the workspace Zetta or a liquid repository (lake#664).
[`web-apps.md`](web-apps.md) defines customer-scoped Ship app creation, direct-zone hosts,
platform-reserved names, deployment, lifecycle, authentication, environment, attach/detach
bindings, logs, events and the read-only app registry pull token. It is separate from identity
[applications](apps.md).
[`builds.md`](builds.md) defines asynchronous in-cluster image builds, configured Ship App runtime
bases, cancellation, status, public digest pull references, retained progress and build-scoped
credentials.

## Arrays in a query string

A read's query values are coerced by the input schema (`rest_input_from_query`,
`src/api/rest.cpp`): an integer, number or boolean property from its text, an object from JSON.
An **array** input (`compare_runs.runs`, `my_permissions.actions`, `get_run_metrics.names`, and any
other) takes, in any mix (lake#576):

- repeated keys, as OpenAPI declares them (`style: form`, `explode: true`):
  `GET /v1/compare_runs?customer_id=<space>&workspace_id=<workspace>&runs=<r1>&runs=<r2>`;
- a comma list:
  `GET /v1/compare_runs?customer_id=<space>&workspace_id=<workspace>&runs=<r1>,<r2>`;
- a JSON array, URL-encoded:
  `GET /v1/compare_runs?customer_id=<space>&workspace_id=<workspace>&runs=%5B%22<r1>%22,%22<r2>%22%5D`.

Each item is coerced by the array's `items` type (`steps=1,2` is `[1, 2]` for integer items), and
an empty value adds nothing, so `runs=` is `[]` and the schema's `minItems` says what is missing.
An item that itself holds a comma can be sent only in the JSON form, or with `POST` and a JSON body.
Before lake#576 only the JSON form worked and repeated keys answered `400` (`expected type
"array"`), so the GET form of every read with an array input could not be called as OpenAPI
described it. `Rest.ArrayInputsDecodeFromRepeatedKeysCommaListsAndJsonArrays` (`tests/test_api.cpp`)
and `Experiments.CompareRunsAnswersItsGetFormWithRepeatedKeysOrACommaList`
(`tests/test_experiments.cpp`) cover it.

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
| `server_info` | read, **no auth** | — | `{name, node, version, revision, api, store, deployments}`; `revision` is the embedded full source commit or `unknown`; `deployments` says whether the platform mirror is configured ([platform.md](platform.md#a-lake-without-the-platform-mirror), lake#605) | [../deployment/production-2026-09-08.md](../deployment/production-2026-09-08.md) |
| `viewer` | read, **no auth** | `workspace_id?`, `customer_id?` | who is signed in, their accounts and current workspace, Clerk, sign-up, origin, agent models, version | [web-app.md](web-app.md) |
| `my_permissions`, `list_packages`, `list_docs`, `get_doc`, `mcp_setup`, `resolve_model` | read (`list_docs`, `get_doc` and a public `resolve_model` need no credential) | see the page | permission flags, the Packages catalog, the Docs guides, Developer → MCP, a model's repository by name | [web-app.md](web-app.md) |
| `latest_agent_task`, `render_pipeline` / `compile_pipeline` | read / action | `customer_id`, `workspace_id`, then `subject`, or `project` and `graph`, or `project` and `intent` | a project's background task, a pipeline drawn as SVG, a pipeline compiled from a description | [web-app.md](web-app.md) |
| `send_ask_message`, `settle_ask_turn`, `confirm_ask_proposal`, `approve_agent_proposal` | action | `customer_id`, `workspace_id`, `conversation_id`, … | the Ask Orama and Agent Factory conversation, written only by lake | [ask-orama.md](ask-orama.md#capabilities-lake561) |
| `delete_ask_conversation` | action, the person who had it | `customer_id`, `workspace_id`, `conversation_id`, `dry_run?` | `{conversation_id, messages, first_message, agent_tasks, dry_run, deleted}` | [ask-orama.md](ask-orama.md#deleting-a-conversation) |
| `transcribe` | action, any signed-in caller | `name`, `audio_base64`, `language?` | `{text, seconds?, model}`: speech-to-text through OpenAI with lake's key; keeps nothing; `not_supported` without `LAKE_OPENAI_API_KEY` | [transcripts.md](transcripts.md) |
| `create_transcript`, `list_transcripts`, `get_transcript`, `update_transcript`, `delete_transcript` | action / read / read / action / action | `customer_id`, `workspace_id`, then `title`, `source`, `text` or `audio_base64`; `q`, `limit`, `cursor`; or `id` | a transcript kept as a sealed `lake_transcripts` record in the workspace Zetta (the audio never kept), a searchable page of them, or `{deleted: true}` | [transcripts.md](transcripts.md) |
| `list_solutions`, `get_solution`, `create_solution`, `update_solution`, `delete_solution`, `duplicate_solution` | read / read / action / action / action / action | `customer_id`, `workspace_id`, then `source?`; `id`; `name`, `problem`, `decision`, `measurement`, `methodologies?`, `pipeline?`, `notes?`; or `id` and `name?` | Orama's catalogue (read-only) and the workspace's own, each marked `source` (`orama` or `workspace`) and `editable`; one of the workspace's own as a sealed `lake_solutions` record; `forbidden` for an edit or delete of Orama's | [solutions.md](solutions.md) |
| `draft_methodology`, `create_methodology`, `get_methodology`, `update_methodology`, `delete_methodology`, `duplicate_methodology`, `validate_methodology`, `run_methodology`, `list_methodology_runs`, `get_methodology_run`, `stop_methodology_run` | read / action / read / action / action / action / action / action / read / read / action | `customer_id`, `workspace_id`, then `description`; `name` and `files`, `description` or `repository_id`; `id`; `id` and changes; `id`; `id`, `name?`; `id`, `dataset?`; `id`, `dataset`, `target?`, `params?`; `methodology_id?`; `run_id`; `run_id` | a workspace's own methodologies, Python in a workspace repository, registered by the engine as `W-<8 hex>` beside Orama's read-only ones (`forbidden` for an edit or delete of an `M-n`); Validate checks the code, then runs it on a sample; runs are untrusted code in the operator's sandbox, `Queued` past the workspace's cap of concurrent runs (orama-operator#293); a run's record is sealed `lake_methodology_runs` | [methodologies.md](methodologies.md) |
| `methodology_run_input`, `methodology_run_artifact`, `methodology_run_output` | read / action / action | `run_id` and `offset?`, `limit?`; or `offset` and `chunk_base64`; or `result`, `artifact_base64?` or `artifact {bytes, sha256}?`, `log?` | the runner's side of a run, answering only that run's `lmr.` credential: its spec and data, page by page; an artifact over 24 MiB in 8 MiB chunks (lake#690); its result, once | [methodologies.md](methodologies.md#it-runs-as-untrusted-code) |
| `get_methodology_deployment`, `list_methodology_deployments` | read / read | `customer_id`, `workspace_id`, then `model_id`; or `limit?` | whether a model a workspace methodology trained is being served (published in Ship → Models), with its phase; `predict` of it answers from its own code (lake#690) | [methodologies.md](methodologies.md#serving-its-model) |
| `methodology_deployment_input` | read | `deployment_id`, `offset?`, `limit?` | the stage side of a served model, answering only that deployment's `lmd.` credential: its spec and the model's artifact, 8 MiB at a time | [methodologies.md](methodologies.md#serving-its-model) |
| `draft_methodology`, `create_methodology`, `get_methodology`, `update_methodology`, `delete_methodology`, `duplicate_methodology`, `validate_methodology`, `run_methodology`, `list_methodology_runs`, `get_methodology_run`, `stop_methodology_run` | read / action / read / action / action / action / action / action / read / read / action | `customer_id`, `workspace_id`, then `description` (`task_id?`: Ask Orama's draft, checked); `name` and `files`, `description` or `repository_id`; `id`; `id` and changes; `id`; `id`, `name?`; `id`, `dataset?`; `id`, `dataset`, `target?`, `params?`; `methodology_id?`; `run_id`; `run_id` | a workspace's own methodologies, Python in a workspace repository, registered by the engine as `W-<8 hex>` beside Orama's read-only ones (`forbidden` for an edit or delete of an `M-n`); Validate checks the code, then runs it on a sample; runs are untrusted code in the operator's sandbox; a run's record is sealed `lake_methodology_runs` | [methodologies.md](methodologies.md) |
| `methodology_run_input`, `methodology_run_output` | read / action | `run_id` and `offset?`, `limit?`; or `result`, `artifact_base64?`, `log?` | the runner's side of a run, answering only that run's `lmr.` credential: its spec and data, page by page; its result, once | [methodologies.md](methodologies.md#it-runs-as-untrusted-code) |
| `create_drive`, `list_drives`, `get_drive`, `rename_drive`, `delete_drive` | action / read / read / action / action | `customer_id`, `workspace_id`, then `name`, `storage`, `repository_id?` or `repository_name?`; or `id` (`confirm_name` to delete) | a drive kept as a sealed `lake_drives` record in the workspace Zetta; a new repository drive gets its first change at once; deleting never deletes a repository | [drives.md](drives.md) |
| `list_drive_entries`, `get_drive_entry`, `create_drive_folder_entry`, `create_drive_shortcut`, `move_drive_entry`, `delete_drive_entry`, `begin_drive_upload`, `append_drive_upload`, `finish_drive_upload`, `upload_drive_entries`, `read_drive_file`, `recent_drive_entries`, `search_drives` | reads and actions | `customer_id`, `workspace_id`, `drive_id`, … | a workspace-files or virtual drive's folders, files and shortcuts; file bytes sealed in the workspace Zetta's `lake_files` bucket, uploaded in 1 MiB-aligned chunks; names found across every drive, a repository drive's paths at `main` included | [drives.md](drives.md) |
| `list_users` | read | `limit` (≤1000, default 100) | `{items: [user]}` | [customers.md](customers.md) |
| `get_user` | read | `id` | user | [customers.md](customers.md) |
| `create_user` | action | `taap_id?`, `email?`, `display_name?` | user | [customers.md](customers.md) |
| `list_organizations` | read | `limit` | `{items: [organization]}` | [customers.md](customers.md) |
| `get_organization` | read | `id` | organization, with its own account's `customer_id` (null when none) | [customers.md](customers.md) |
| `create_organization` | action | `taap_id?`, `display_name?`, `customer_id?` | organization | [organizations.md](organizations.md) |
| `delete_organization` | action, an owner | `organization_id`, `confirm` (its handle), `dry_run?` | what went, with counts; `409` naming the workspaces it still owns | [organizations.md](organizations.md#deleting-an-organization) |
| `revoke_invitation`, `revoke_grant` | action, `admin` on the scope or object | `invitation_id` or `grant_id`, `dry_run?` | the invitation (now `revoked`) or the grant removed | [organizations.md](organizations.md#revoking-an-invitation), [policy.md](policy.md#revoking-a-grant) |
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
| `model_traffic` | read | `customer_id`, `workspace_id`, `model_id?`, `from_ms?`, `to_ms?`, `step_ms?` | each model's `predict` calls, errors and mean latency, step by step, from the workspace's own series; the workspace's `predict` requests; drift not measured yet (lake#599) | [workspaces.md](workspaces.md) |
| `get_model`, `get_dataset`, `resolve_model_file`, `resolve_dataset_file` | read | repository id and revision, or, without `revision`, a workspace model or dataset id with `customer_id` and `workspace_id`; file path where required | compatible model/dataset metadata, the workspace's trained model or dataset schema, storage/provenance, or exact file | [models.md](models.md) |
| `delete_dataset`, `delete_model`, `delete_run`, `delete_index` | action | `customer_id`, `workspace_id`, `id`, `dry_run?` | the engine's account of what went (`removed`, `kept`); `501 not_supported` from an older engine; `delete_dataset` refused while a run or a study still reads the dataset (zetta#1258) | [models.md](models.md#deleting-from-a-workspace) |
| `train_model`, `retrain_model`, `register_model`, `move_model_to_project` | action | `customer_id`, `workspace_id` and the engine's fields: a name, methodology, dataset and target; a model id and new settings; a model file's text; a model id and project name | a training run's `run_id`, a brought model's `model_id`, or the project; `501 not_supported` from an older engine | [models.md](models.md#a-standalone-model-lake654) |
| `load_drive_model` | action | `customer_id`, `workspace_id`, `drive_id`, `id`, `name` or `version_of`, `kind?`, `metrics?` | `register_model`'s answer with the file's `source` | [drives.md](drives.md#a-drive-file-as-a-model-lake654) |
| `prepare_repository_upload`, `stage_repository_file`, `commit_repository_files` | action | repository id plus upload plan, verified staged bytes, or file operations | upload modes, unique stored bytes, or immutable committed revision | [models.md](models.md) |
| `attach_model_provenance`, `verify_model_provenance` | action | model and dataset revisions plus canonical liquid attestations, or model revision | attached or re-verified dataset-to-model training chain | [models.md](models.md) |
| `create_repository` | action | `customer_id`, `workspace_id`, `name`, `description?`, `visibility?` | repository, with its `workspace_id` | [repositories.md](repositories.md) |
| `rename_repository`, `transfer_repository`, `set_repository_visibility`, `delete_repository` | action | repository id plus the lifecycle field where applicable; `transfer_repository` is always refused (a repository stays in its workspace) | repository | [repositories.md](repositories.md) |
| `refs` | read | `repository_id` | `{items: [repository ref]}` | [sync.md](sync.md) |
| `find_missing`, `push`, `pull`, `upload_chunks`, `read_chunk` | binary read/action | repository id plus liquid sync body, digest, or range | binary liquid response; push reports every reference outcome and refusal reason | [sync.md](sync.md) |
| `create_issue`, `update_issue`, `list_issues`, `create_issue_comment`, `list_issue_comments`, `issue_assignees` | read/action | repository id, issue fields, state, or issue id | durable Markdown issue and discussion records with their author; the people an issue can be assigned to | [forge.md](forge.md) |
| `create_review`, `update_review`, `list_reviews`, `create_review_comment`, `list_review_comments`, `map_review_anchors` | read/action | repository id, liquid change, review state, anchor, or text hunks | change review and cursor-compatible anchored discussion records | [forge.md](forge.md) |
| `create_release`, `list_releases` | read/action | repository id, tag, change, and build-result IDs | tag release retaining verified attestation evidence | [forge.md](forge.md) |
| `create_webhook`, `list_webhooks`, `deliver_webhook`, `retry_webhook_delivery`, `list_webhook_deliveries` | read/action | repository id, subscription, endpoint, event, payload, or delivery ID | signed delivery attempts and retained retry log | [forge.md](forge.md) |
| `set_branch_protection`, `list_branch_protections` | read/action | repository id, ref pattern, review/verification/fast-forward requirements, and allowed pushers | ref policy records and policy-object binding; enforcement waits on liquid #177 | [forge.md](forge.md) |
| `export_repository`, `import_repository`, `list_exports` | read/action | repository or destination customer and workspace, archive, and optional Git projection | signed portable archive, recreated repository, or prior export records | [export.md](export.md) |
| `import_github_repository` | action | destination customer and workspace, local Git directory, and recorded GitHub responses | imported repository and durable fidelity report | [github-import.md](github-import.md) |
| `list_teams`, `get_team`, `create_team` | read/action | organization or team identifiers and fields | team(s) | [organizations.md](organizations.md) |
| `list_memberships`, `get_membership`, `create_membership`, `set_member_role`, `remove_member` | read/action | scope (omit for your own), membership id, principal, role; lake signs the evidence (lake#602) | membership(s) or removal confirmation | [organizations.md](organizations.md) |
| `list_invitations`, `create_invitation`, `accept_invitation` | read/action | scope (omit for your own), invitee, role, expiry or accepting principal | invitation or membership | [organizations.md](organizations.md) |
| `create_organization_space`, `convert_personal_space`, `join_organization` | action | organization name and handle, your personal account, or an invitation addressed to you | organization account or membership | [organizations.md](organizations.md) |
| `list_objects`, `get_object`, `create_object` | read/action | graph identifiers, type, parent, `inherit` | object(s) | [policy.md](policy.md) |
| `list_grants`, `create_grant` | read/action | object, principal, role; lake signs the evidence (lake#602) | grant(s); a created one with `attestation` | [policy.md](policy.md) |
| `effective_access` | read | `object_id` | `{items: [effective access]}` | [policy.md](policy.md) |
| `entity_effective_access` | read | `object_type` (`repository`, `organization`, or `team`), `id` | `{object, items: [effective access]}` | [policy.md](policy.md) |
| `identity` | read, **no auth** | — | lake TAAP identity document | [audit.md](audit.md) |
| `rotate_identity` | action | — | replacement lake identity document | [audit.md](audit.md) |
| `audit` | read | `principal?`, `object?`, `outcome?`, `from?`, `to?`, `limit?`, `workspace_id?`, `customer_id?`, `log?` | `{items: [audit event with log], unread_workspaces}`, the central log and the workspaces' audits | [audit.md](audit.md#reading-both-logs) |
| `export_audit` | read | audit filters | `{jsonl}`, the central log or one workspace's audit | [audit.md](audit.md) |
| `verify_audit` | read | `workspace_id?`, `customer_id?` | `{verified: true, log}`, the central chain or one workspace's | [audit.md](audit.md#tamper-evidence) |
| `set_audit_retention` | action | `customer_id`, `days` (≥400) | `{customer_id, days, workspace_audit_retention}` | [audit.md](audit.md#retention) |
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
| `list_agent_library` | read | none | the Agent Library: every specialized agent Orama ships, whether this lake runs it, and, only with `include_internal: true`, Orama's own tooling marked `internal` (never offered in Ask Orama) | [train-agents.md](train-agents.md) |
| `list_agent_revisions` | read | customer, workspace, agent, optional limit | every revision of an agent's definition, newest first: its number, who made it and when, `create` or `update`, and the definition it set (lake#653); restore one with `update_agent` | [train-agents.md](train-agents.md#definition-revisions) |
| `list_datasets` | read | customer | The workspace dataset catalogue with stable `<database>/<name>` ids (the form `get_dataset` takes) and `kind` (`data` or engine `result`) | [workspaces.md](workspaces.md) |
| `list_runs` | read | customer, optional project/solution and limit | newest matching runs with status, reached stage and final error line | [workspaces.md](workspaces.md) |
| `launch_agent`, `list_agent_launches`, `stop_agent_launch`, `schedule_agent`, `list_agent_schedules`, `unschedule_agent` | action / read | customer, agent, mode, input, cron or trigger | run, follow and stop Python agents on agent-runtime, and give them cron schedules (each a CronJob) | [agent-launches.md](agent-launches.md) |
| `set_agent_streams`, `list_agent_streams` | action / read | customer, workspace, agent, streams | the workspace streams whose new messages each start a job of a Python agent, as the person who set them (lake#677) | [agent-launches.md](agent-launches.md#stream-triggers) |
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
| `usage_over_time` | read | `customer_id`, `workspace_ids?`, `dimensions?`, `from`, `to`, `granularity` (`hour`, `day`, `month`) | each dimension of usage over the period, added up across the chosen workspaces, with breakdowns by workspace and kind (and by agent, launch, model or object where they apply), read from each workspace's own Zetta (lake#564); what an engine holds on its own volume is `engine_storage`, detail, never billed (lake#663) | [metering.md](metering.md#usage-over-time) |
| `usage_over_time` | read | `customer_id`, `workspace_ids?`, `dimensions?`, `from`, `to`, `granularity` (`hour`, `day`, `month`) | each dimension of usage over the period, added up across the chosen workspaces, with breakdowns by workspace and kind (and by agent, launch, model or object where they apply), read from each workspace's own Zetta (lake#564); app traffic through the ingress is `transfer` once its hour closes (lake#661) | [metering.md](metering.md#usage-over-time) |
| `usage_limits`, `set_usage_limit` | read/action | `customer_id`; a `dimension`, `limit` and `warn_percent?` | the account's limits with this month's use and a state (ok, warning, over); storage's limit is the enforced quota (lake#564) | [metering.md](metering.md#limits) |
| `price_list`, `set_price_list`, `billing_account`, `bill_preview`, `issue_invoice`, `list_invoices`, `get_invoice`, `record_invoice_payment`, `void_invoice` | read/action | the price list's rates and `effective_from`; a `customer_id`, `period` (`YYYY-MM`) or `invoice_id` | Orama Cloud's billing, registered only where `LAKE_BILLING` is on (absent on enterprise installs): one price list with a rate per usage dimension that every account is billed at (lake#635; time per minute, lake#637), the month's bill from its usage, invoices and their payment state, in PostgreSQL (decision 56, lake#564) | [billing.md](billing.md) |
| `record_model_usage` | action | `model_name`, `input_tokens`, `output_tokens`, `task_id?` | an agent's own task, launch, schedule or bus credential reports the tokens of a model call made outside Lake; lake adds them to that workspace's usage (lake#564) | [metering.md](metering.md#model-tokens) |
| `model_completion` | action | `endpoint` (`chat/completions`, `responses`, `embeddings`), `request` (the OpenAI body) | the model gateway: an agent's task, launch or schedule credential calls the model provider through Lake with Orama's key; the provider's answer, its tokens counted as the run's workspace's `model_tokens`, refused past the monthly limit; also served OpenAI-compatible at `POST /openai/v1/<endpoint>` (lake#248) | [model-gateway.md](model-gateway.md) |
| `create_secret_mount`, `list_secret_mounts`, `get_secret_mount` | action/read | customer, `workspace_id`, optional organization (without it the account owns the mount, lake#561), mount fields | mount metadata with `owner_type` (in the mount's workspace Zetta) | [secrets.md](secrets.md) |
| `delete_secret_mount` | action, `admin` on the mount | `mount_id`, `confirm` (its name), `dry_run?` | counts of what went from the workspace Zetta, and the secrets' names | [secrets.md](secrets.md#deleting-a-mount) |
| `put_secret`, `read_secret`, `secret_metadata` | action/read | mount, name, value/version/metadata fields | versioned value or history | [secrets.md](secrets.md) |
| `list_secrets` | read | mount, `prefix?`, `cursor?`, `limit?` | a mount's secret paths with current version, times and deleted state; never values | [secrets.md](secrets.md) |
| `delete_secret_versions`, `undelete_secret_versions`, `destroy_secret_versions` | action | mount, name, `versions[]` | updated history | [secrets.md](secrets.md) |
| `lease_dynamic_secret`, `renew_dynamic_secret`, `revoke_dynamic_secret` | action | mount, role and bounded lifetime; then lease id | response-only generated credential or updated lease metadata (the lease lives in the mount's workspace Zetta, lake#362) | [secrets.md](secrets.md#dynamic-leases) |
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
