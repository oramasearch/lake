# CLAUDE.md — lake

The lake binary. **Org-wide conventions** live in
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge) `CLAUDE.md`
and apply in full. Project conventions live in
[`oramasearch/liquid`](https://github.com/oramasearch/liquid) `CLAUDE.md` — vocabulary,
document rules, confidentiality, and the decisions not to drift past. Read both.

## Status

Status: 2026-10-17. The current release is **v0.2.0**
([`docs/releases/v0.2.0.md`](docs/releases/v0.2.0.md): secure networking, regions, the managed key
and own identity, Clerk webhooks, the agents' names and Ask Orama's home, with its known issues).
`server_info` of a v0.2.0 build reports version `0.2.0`. The paragraph below describes v0.1.0, the
first tagged release ([`docs/releases/v0.1.0.md`](docs/releases/v0.1.0.md) lists everything in it),
and what ran on 2026-10-12. `Lake/prod` on the
dev AKS cluster (there is no production environment yet) serves `oramasearch/lake:20261012a`, which
is also `oramasearch/lake:v0.1.0`, built from the tag (`2e6f6d7`) against liquid `v0.1.0`
(`f669726`). `GET /v1/server_info` reports version `0.1.0`; the OpenAPI lists 554 paths and the
operator's schema is applied through `0083_methodology_deployments`. Its bus and workspace Zettas
run `oramasearch/zetta:main-4015ef76` (zetta `v0.2.0`).

Secure-networking desired state is implemented in the source tree (lake#727, `0084_network`):
four desired/status pairs, scoped stores, signed atomic audit and app-to-service rollout rows.
Tunnel grants end through revocation or expiry; internet egress stays implicit in the operator.
Fourteen permission-checked capabilities expose connections, scoped credential rotation, egress,
tunnel grants, recipient-only CSR signing, peers and effective rules through REST, OpenAPI and MCP
(lake#728, lake#730, lake#731).
Migration `0085_network_tunnel_ca` adds public client/relay CA fields without changing 0084.
Session and activity reads return empty arrays with `pending: "zetta#1281"` through a provider
interface. Operator adoption, live relay session termination and deployment are not claimed. See
[`docs/control-plane/network.md`](docs/control-plane/network.md) and
[`docs/api/network.md`](docs/api/network.md), [scoped credentials](docs/api/scoped-credentials.md)
and [tunnel credentials](docs/api/tunnel-grants.md).
Each workspace has a Network section in the console after Ship (lake#729): Overview, Connections,
Developer Access, Private Networks, Egress and DNS, Policies and Activity, which together call all
fourteen capabilities. Data → Connections (datasets) stays separate and each links the other. Not
deployed. See [`docs/ui/network.md`](docs/ui/network.md) and the [Network guide](docs/guide/network.md).
Lake serves the tunnel client itself (lake#745 part 2, no migration): the image copies the
`zetta-tunnel` binaries (macOS arm64, Linux amd64) from the private `oramasearch/zetta-tunnel-clients`
image pinned by digest to `LAKE_TUNNEL_CLIENTS_DIR` (default `/usr/share/lake/tunnel-clients`);
`tunnel_client_downloads` answers, to any signed-in person, the version and per platform the SHA-256,
size and a 15-minute link signed with the Hub upload links' HMAC under `LAKE_INSTANCE_KEK`, which
`GET /tunnel-clients/<platform>` serves without a session; Get credentials makes installing it step 4.
Not deployed. See [`docs/api/tunnel-clients.md`](docs/api/tunnel-clients.md).

Every new account starts with a managed key, and a person may use their own identity instead
(lake#762, liquid-bridge decision 65, migration `0086_identity_links`): Lake's own password sign-up
no longer needs an identity document and mints a managed key as a first Clerk sign-in does;
Account → Identity (`/settings?tab=identity`) reads `account_identity` and runs *Use my own
identity*: `begin_own_identity` verifies the `liquid identity show --document` output (an initial
document only) and issues a ten-minute challenge, `use_own_identity` verifies the
`liquid sign artifact <challenge>` signature with libliquid, has the managed key sign the link, and
in one transaction records it and destroys the managed seed. The account keeps its identifier, so
grants, audit and repositories stay; `sign_in_taap` with the own key reaches it, and a push's
`push_change` audit names the signer and the account it acts for. Not deployed; the operator must
vendor the schema and select `identity_links_guard`. See
[`docs/api/identity.md#your-own-identity`](docs/api/identity.md#your-own-identity).

Clerk webhooks follow (lake#765, migration `0087_clerk_webhooks`): `POST /identity/clerk-webhook`,
verified with Lake's own Svix HMAC and `LAKE_CLERK_WEBHOOK_SECRET` (404 without it), applies each
`svix-id` once: `user.created` provisions the account as a first Clerk sign-in does, `user.updated`
sets its verified email and name, `user.deleted` disables it (`users.disabled_at`) and revokes its
sessions without erasing anything; every delivery is audited centrally. Not deployed; the operator
must vendor the schema, select `clerk_webhooks_guard` and configure the endpoint and secret
(orama-operator#541). See [`docs/api/clerk.md#webhooks`](docs/api/clerk.md#webhooks).

The newest dated record, [`docs/deployment/dev-2026-10-01.md`](docs/deployment/dev-2026-10-01.md),
and [`docs/design/status.md`](docs/design/status.md) still describe the 2026-10-01 deployment
(`oramasearch/lake:20261001d`, `36679e1`); they have not been brought up to the release. Until they
are, the record is authoritative only for what was exercised on that date.
`status.md` distinguishes implemented, exercised locally, deployed, and exercised-in-deployment
behavior; do not infer an end-to-end deployment exercise from a route's presence in OpenAPI.
`tools/docs/check-status.py` (run by `DocsStatus.MatchesTheObservedOpenApi`) fails when that page
calls a capability undeployed that the newest snapshot in `docs/deployment/observed/` lists; after
a deployment, record a new snapshot and update the page.

Train → Experiments is implemented in the source tree with eleven registered REST/MCP operations,
live charts, confirmed promotion, and the Experiments Agent's editable proposal and confirmed start,
against workspace Zetta `dev-a1d74b21`. The build's authorized bridge transport source is
unchanged. Deletion has no engine contract; deployed experiment/training acceptance is not claimed.
Lake says experiment (`get_experiment`, `start_experiment`, `stop_experiment`, `experiment_id`) where
the engine says study, translating at its boundary (lake#561, not deployed); the guides and pages
never say study. See `docs/api/experiments.md` and `docs/ui/experiments.md`.

The console is the web app in `frontend/` (lake#561), embedded in the binary and answered for every
browser page request; it calls only `/v1`. The server-rendered console pages, their deferred panels
and HTML form handlers, the `lake_workspace` cookie and the experiment chart assets are retired in
the source tree; an identity application's hosted sign-in pages and the apex host's region picker
(lake#757, `docs/api/regions.md`) are the server-rendered surfaces. `agent_turn` checks a confirmed Application Agent or Experiments Agent proposal against the
stored one (`proposal_task_id`, `proposal_sha256`). Not deployed yet. See `docs/ui/web-app.md` and
`docs/serving/http.md`.

Speech-to-text and workspace transcripts follow (lake#561, no migration): `transcribe` sends audio
to OpenAI (`gpt-4o-transcribe`) with lake's key (`LAKE_OPENAI_API_KEY`, `LAKE_TRANSCRIBE_MODEL`)
through the server's outbound client, an injected `Transcriber` the tests replace with a fake, and
`not_supported` without a key; `create_transcript`, `list_transcripts` (`q` on title and text),
`get_transcript`, `update_transcript` and `delete_transcript` keep sealed `lake_transcripts`
records in the workspace Zetta, from a recording, uploaded audio, a cleaned `.vtt`/`.srt` or pasted
text. The audio is never kept. Not deployed; `Lake/prod` needs the key in `spec.env`
(`docs/deployment/aks.md#speech-to-text-key`). See `docs/api/transcripts.md`.

Drives follow (lake#561, no migration): a workspace's drives are sealed `lake_drives` records that
keep files in a repository (a new one gets its top folder marker as its first change, so `main`
exists for every drive_* capability), as workspace files, or as shortcuts only (virtual). Workspace
files are `lake_drive_entries` records, and each file's bytes one object under an opaque key in the
workspace Zetta's `lake_files` bucket, sealed in 1 MiB AES-256-GCM blocks with nonces derived from
each block (keys from `LAKE_INSTANCE_KEK` per workspace, not the workspace store's data key), so a
resent chunk appends identical bytes, appends are durable, the bucket takes no lease, and a move
never copies bytes. Uploads go in 1 MiB-aligned chunks (`begin_drive_upload`, `append_drive_upload`,
`finish_drive_upload`, which records the SHA-256 read back); `GET /drive/files/download` streams a
file after `get_drive_entry` authorizes it. Shortcuts to repositories, repository paths, drive
items and transcripts say when they break. Data → Drives is one file manager over a
`DriveStorage` interface with three implementations. `lake_files` has no quota yet. Not deployed.
See `docs/api/drives.md` and `docs/ui/drives.md`.
The 2026-10-10 review's drive fixes follow (no migration): drive writes and pushes publish branch
tips into a space's hot search index too (lake#584); a folder is shared by email or handle among
the space's people (`drive_share_people`) and lake signs the grant's attestation, refusing a
made-up digest (lake#585); `download_drive_file` answers the bytes and a `download_url` (lake#587);
a resent chunk answers the true `received`, and unfinished uploads are listed and expire after
24 hours (lake#588); `load_drive_dataset` loads a drive's CSV or JSONL file (lake#593); the web app
retries an edge 503 read under the one `shouldRetry` rule and names who answered (lake#573,
`docs/ui/web-app.md#a-read-answered-503`).
Data → Connections (lake#651, no migration): the databases a workspace reads have their own page
(`/connections`, one connection's tables at `/connections/connection?name=`), out of Datasets,
whose Load data picks from them; old `?browse=`/`#connections` links redirect
(`docs/ui/connections.md`). Storage moves from Datasets to the Storage tab of Data → Drives, and a
bucket or filesystem store opens as a fourth drive kind, `store` (lake#656, no migration):
`create_drive {storage: "store", store_id}` and the entry capabilities browse it in place through
`control::StoreContents` and the injected `StoreBrowser` (an item's id is its path; deleting the
drive never deletes the store; a volume is refused). Every bucket and filesystem pair is offered
and browsable (lake#664, no migration): the customer's own bucket (`r2`, `s3`, `b2`, `gcs`, `azure`)
named with a secret of the workspace's secret store (decision 58, never Orama's) and reached by
Lake's own S3 (SigV4) and Azure Blob clients (no SDK) over a public-only transport
(`control::cloud_store_browser`); a `zetta` store kept as sealed `lake_store_items` records and a
sealed `lake_st_<hex>` bucket in the workspace Zetta; a `lake` store on a liquid repository's
`main`. One conformance suite runs against each over a local fake. Not deployed: the operator must
reconcile them first (orama-operator#294). Which services and apps mount a store is not
recorded (lake#666). Not deployed. See `docs/api/drives.md#store-drives` and
drive never deletes the store; a volume is refused). No provider is browsable yet, so without a
browser it answers `not_supported` (lake#664). Which services and apps mount a store is recorded
(lake#666, migration `0080_store_mounts`): `stores` on `create_service`, `set_service_stores`,
`create_app` and `set_app_stores`, one workload per volume, a mounted store not deleted, and
`list_stores` answering `mounted_by` and `used_bytes`; the operator must vendor the schema, mount
the stores and report bytes in use. Not deployed. See `docs/api/drives.md#store-drives` and
`docs/ui/drives.md#storage`.

A workspace's own AI solutions follow (lake#597, no migration): sealed `lake_solutions` records in
the workspace Zetta in the catalogue's shape (name, business problem, decision, measurement with
metric, baseline and target, composed methodologies, pipeline template or reference, notes).
`list_solutions` and `get_solution` answer Orama's catalogue (read through the workspace's engine)
and the workspace's own, marked `source: orama|workspace`; `create_solution`, `update_solution`,
`delete_solution` and `duplicate_solution` change only the workspace's own, and an edit or delete of
Orama's answers the new `forbidden` (403). Lake's `create_solution` is not the engine's, which
`confirm_solution` still calls inside `workspace_act`. Start as a Project for one of your own creates
the project and its first pipeline from the composed methodology, since the engine composes the
governed foundation only for catalogue ids. Not deployed. See `docs/api/solutions.md`.

A workspace is the client of remote MCP servers (decision 60, lake#652, no migration): Lake's own
client (`McpClient`, Streamable HTTP and HTTP+SSE, every exchange bounded) over the server's
transport, which reaches public addresses only (`LAKE_MCP_ALLOW_PRIVATE_ADDRESSES=1` for local
development); each connection a sealed `lake_mcp_connections` record in the workspace Zetta, its
bearer token or OAuth refresh token a workspace secret Lake reads server-side and never answers;
fifteen capabilities (`create_mcp_connection` … `create_mcp_connection_agent`), a tool allowed by a
person and either a read or a change, a change a person's or an auto-approved agent's (per tool and
agent, audited), calls audited in the workspace and metered (`calls`, `transfer`); OAuth 2.1 sign-in
(RFC 9728/8414 discovery, RFC 7591 registration, PKCE S256, refresh rotation); opt-in re-export
through `/w/<workspace>/mcp` as `<prefix>__<tool>`; a connection's agent (skill `mcp:<prefix>`,
needing the zetta change) with input and output streams and mirrored notifications, run by the store
root's owner (`McpStreamWorker`); Data → MCP Connections in the web app. Not deployed. See
`docs/api/mcp-connections.md` and `docs/ui/mcp-connections.md`.
A standalone model follows (lake#654, no migration): Train → Models' New model trains one from a
dataset, a target and a methodology (`train_model`) or brings a model file from this computer, a
drive (`load_drive_model`) or a model repository (`register_model`); a standalone model has Retrain,
New version (`retrain_model`), Move into a project (`move_model_to_project`) and Delete (each
version's `delete_model`), and Ship serves it as any model. The engine keeps an internal project for
it that it never lists. An engine that lacks an allow-listed capability is answered `not_supported`.
Needs the zetta standalone-models change in the workspace image. Not deployed. See
`docs/api/models.md#a-standalone-model-lake654` and `docs/ui/train.md#a-standalone-model`.
Bring one takes common model formats and any version's weights download (lake#681, no migration):
`register_model` and `load_drive_model` take ONNX tree ensembles (scikit-learn, XGBoost, LightGBM),
XGBoost JSON/UBJSON and LightGBM text models as `weights_base64`, which the engine converts exactly
into its own trees (`feature_names` names an ONNX model's columns); pickles, neural networks and
random-forest classifiers are refused. `export_model` (a named engine read, audited) downloads a
version's weights as kept or as ONNX; Train → Model has a Weights card. Needs the zetta model-import
change in the workspace image. Not deployed. See `docs/api/models.md#bring-a-model-in-a-common-format-and-download-its-weights-lake681`.

The workspace engine's capabilities are named capabilities (lake#650, no migration): every name in
`workspace_reads` and `workspace_actions` is served by `/v1/<name>`, its own OpenAPI operation and
its own MCP tool, either one registered from the engine's input schema (`src/api/engine_openapi.json`,
vendored from Zetta's `schemas/openapi.json` by `tools/api/vendor-engine-openapi.py`) or a Lake
capability that already covered it. Each runs the `workspace_query`/`workspace_act` path
(`call_workspace_capability`), so policy, agent scopes, audit and caching are unchanged; Lake renames
where the engine's name collides or says too little (`gate` is `check_pipeline`, `run` is
`start_run`, `search` is `search_workspace_index`). Those two stay, with an `enum` on `capability`.
Not deployed. See `docs/api/engine-capabilities.md`.
Agents are designed in Design → Agents and trained in Train → Agents (lake#653, no migration):
Design → Agents (was Agent Library; `/design/agent-library` and `/train/agents/new` redirect) lists
Orama's agents read-only beside the workspace's own, with New agent, Duplicate, Edit and the Agent
Factory; every `create_agent` and `update_agent` the engine accepts is kept as a sealed, append-only
`lake_agent_revisions` record in the workspace Zetta (`list_agent_revisions`); Train → Agents lists
only agents with something to train, with performance, versions, promote and roll back, the
training schedule and Launch experiment. The pages call the named engine capabilities. Not
deployed. See `docs/api/train-agents.md`.
An agent can be started by a workspace stream (lake#677, no migration): `set_agent_streams` names
the workspace streams whose new messages each start a job of a Python agent with the message as its
input, as the person who set them; each trigger is a sealed `lake_agent_stream_triggers` record in
the workspace Zetta, read by the store root's owner (`AgentStreamWorker`, beside the MCP connection
streams, through the same `start_stream_runs`), and removed with its agent by `delete_agent`. Design
→ Agents' New agent and Edit offer the streams; the agent's Design page lists them. Not deployed. See
`docs/api/agent-launches.md#stream-triggers`.

A workspace's own methodologies from Python code follow (lake#655, migration `0078_methodology_runs`):
the code is a manifest, an entrypoint with fit, predict and score, and a hashed lock file in a
workspace repository; the engine registers it as `W-<8 hex>` beside Orama's (zetta#1261), whose edits
and deletes answer `forbidden`. Validate checks the code without running it, then builds its image by
the build path (`LAKE_METHODOLOGY_RUNTIME_IMAGE`) and runs it on a sample. Each run is a
`methodology_runs` dispatch row the Orama operator runs as stage, run and collect pods
(orama-operator#291): the code holds no credential and has no network unless granted; the run's
`lmr.` credential reaches only `methodology_run_input` and `methodology_run_output` (and a pull of its
image); its record is sealed `lake_methodology_runs` in the workspace Zetta; a training run's model
is registered by the engine; a pipeline's `runner_required` run is dispatched to it; finished runs are
metered as `methodology_run`. Not deployed; the operator must vendor the schema and select
`methodology_runs_guard`, and `oramasearch/methodology-runtime` must be published. See
`docs/api/methodologies.md`.
Serving a workspace methodology's model and artifacts over 24 MiB follow (lake#690, migration
`0083_methodology_deployments`): publishing a `python:W-…` model in Ship → Models writes a
`methodology_deployments` row the operator runs as a stage Job (the `lmd.` credential,
`methodology_deployment_input`) and a sandboxed `serve` Deployment only Lake's pods reach; `predict`
is forwarded there through the injected `MethodologyServer`; the serve pods are metered as
`methodology_deployment`; collect sends an artifact up to 1 GiB through `methodology_run_artifact`.
Not deployed; needs the zetta, operator and methodology-runtime changes of lake#690 and the operator
selecting `methodology_deployments_guard`.
The operator queues a workspace's methodology runs past the Lake's
`spec.methodologies.maxConcurrentRunsPerWorkspace` (default 2) and places a GPU run's work volume in a
zone where its GPU class has nodes (orama-operator#293, migration `0082_methodology_run_queue`, guard
`methodology_run_queue_guard`): phase `Queued` with the operator's reason, shown in Design →
Methodologies; a queued run is never withdrawn as unreported. Not deployed; the operator vendors the
schema and selects the guard in the same issue.
New Methodology's first draft is written by Ask Orama (lake#688, no migration): an `agent_turn` of
kind `methodology_draft` to agent-ask-orama, whose answer `draft_methodology {task_id}` offers only
when the manifest, entrypoint and lock checks pass, otherwise Lake's template with the reason. Needs
agent-ask-orama with that turn kind. Not deployed. See `docs/api/methodologies.md#drafts`.
`docs/api/methodologies.md`. Experiments over its parameters follow (lake#689, no migration): the
engine samples its declared parameters and lists the waiting trials as `runner` work; Lake queues
each as a training run (claimed once in the sealed `lake_methodology_trials` record), reports the
result with the experiment and trial, ends a trial whose run failed, and stops the runs with the
experiment. Needs the zetta lake#689 change. See `docs/api/methodologies.md#experiments`.

Every future deployment image receives OCI revision and creation labels. The binary reports the
same embedded revision through `GET /v1/server_info`; details are in
[`docs/build/container-image.md`](docs/build/container-image.md).

The source tree hands the store root over between processes (lake#479): a starting lake serves
probes and reports Ready while a predecessor holds `.lake-owner.lock`, holds store-touching requests
until it owns the root, and on SIGTERM drains, closes its stores (opened with liquid's
`exclusive_owner`, liquid#322) and releases the lock. It needs liquid at or after `df2ae4d`. The
operator's `spec.rollout.overlapOnSharedStore` can be turned on only once a deployed image contains
it. See `docs/deployment/store-volume.md`.
Workspace stores are handed off per workspace (lake#536): at SIGTERM lake answers new requests to a
workspace store `503` with `Retry-After` and releases each store's lease once its admitted requests
finish, and a starting lake pre-opens every running switched workspace's store in the background,
taking each lease the moment it is released, but only once its predecessor has announced its drain
in `<root>/.lake-owner.draining` or it owns the root (`LAKE_WORKSPACE_STORE_PREOPEN`). A workspace
stopped or deleted while an attempt waits for its lease is checked again once the lease is taken,
and its store closed and lease released at once (lake#541). See
`docs/architecture/workspace-store.md#rollout-hand-off-lake536`.

Agent tasks now have renewable claim leases, two retries and a terminal failure after three
expiries. Retry notices reach workspace event streams and the working panels. Shared bus
credentials retain first-terminal-write-wins behavior; task-scoped event authentication and
operator schema/configuration handoff remain follow-ups. See `docs/api/agent-bus.md`.

Lake's records move into workspace Zettas through one mechanism,
[`docs/architecture/workspace-records.md`](docs/architecture/workspace-records.md): sealed
`lake_*` collections in each workspace Zetta's keyed record store (zetta#1231, Zetta dev with
#1233 or later), an operator one-shot copy and verification, and a per-table checklist. Ask Orama conversations are the first collection (lake#370,
migration `0045`); Design → Opportunities are the first mutable one (lake#376, migration `0046`).
Topic creators follow (lake#379, migration `0047`). Drive share links are the first mutable collection (lake#350, migration `0048`): their bearer
token names its workspace, and only a content-free route for pre-move tokens stays central.
Application lifecycle events and webhook delivery attempts follow (lake#356,
lake#357, migration `0049`, in the application's workspace), and its signing keys and webhooks with
their secrets (lake#352, lake#355, migration `0056`: one key ring per application rotated by one
conditional write; seeds and secrets stay secrets envelopes inside the seal; the anonymous JWKS
finds the workspace by the application id). Its settings follow (lake#351, migration `0059`): name,
origins, redirect URIs and theme are one sealed record in the application's workspace, and
`auth_applications` stays as the content-free routing row (id, space, pool, custom sign-in host,
workspace). Its one-use authorization codes follow (lake#363, migration `0061`): issued `if_absent`
in the application's workspace, redeemed once by a delete at the revision read (the relying party's
exchange finds the workspace from the routing row), and expired by the collection's five-minute
retention.
Every retired table and content column was copied and verified on dev, and migration
`0070_drop_moved_tables` (lake#311) drops them: `opportunities`, `stream_owners`,
`drive_share_links`, the `auth_app_*` tables, `metering_*`, `forge_records`, the `secrets_*` tables,
`platform_*`, and the content columns of `auth_applications`, `agent_tasks`/`agent_task_status` and
`repositories`. A first block refuses rows with no recorded workspace owner (copy and verify, with
an image built before `0070` such as `20261005a`, are a manual gate before applying it); audit, routing rows, routes and `keystore` stay.
`copy_workspace_records` now accepts only `audit_events` and `oci_tags`. See
`docs/control-plane/schema.md#drop-moved-tables-migration`.
Repository usage follows (lake#348, lake#349, migration `0053`): exact hourly transfer counters and
one build record per result in the repository's workspace Zetta, written from a bounded in-process
buffer the owner loop flushes every five seconds and on drain, never on the sync path; repository
storage is measured live and no longer recorded. See `docs/api/metering.md`.
Secrets follow (lake#328-#332, migration `0057`): a secret mount belongs to one workspace, chosen
at creation, and its secrets, versions, named keys and data keys are sealed records in that
workspace's Zetta with the existing envelope (instance KEK, customer KEK, data key); PostgreSQL
keeps only the content-free `secret_mount_routes`, the policy objects and the customer KEK. Version
numbers are claimed `if_absent`, and a customer KEK rotation rewraps every workspace's data keys
before it forgets the replaced KEK. See `docs/api/secrets.md`.
A mount's owner is the account that owns its workspace or, as before, an organization inside it
(lake#561, migration `0071_secret_mount_owners`): `create_secret_mount` without `organization_id`
needs `admin` on the customer, hangs the mount under `customer → environment secrets/<customer>`,
claims its name for `customer:<id>`, and stores a NULL organization in its route. Not deployed; the
operator must vendor the schema and select `secret_mount_owners_guard`. See
`docs/api/secrets.md#who-owns-a-mount-lake561`.
Usage over time follows (lake#564, migration `0072_usage_limits`): every dimension of a workspace's
usage is kept in its workspace Zetta, the counter `lake_usage` (API and MCP calls, model
predictions, speech-to-text seconds, model tokens, instance and volume seconds) and the gauge
`lake_storage` (repositories, images, workspace data, transcripts, drive files), an hour a bucket for
400 days, written by the bounded `UsageRecorder` the owner loop flushes and sampled every fifteen
minutes by the store root's owner. `usage_over_time` adds the chosen workspaces up by hour, day or
month with breakdowns by workspace and kind; `usage_limits` and `set_usage_limit` keep content-free
per-dimension limits in `customer_usage_limits` (storage's stays the enforced quota; the others
warn); `record_model_usage` takes an agent's own credential. Usage is a dashboard in the web app. No
billing here. See `docs/api/metering.md#usage-over-time`.
Billing follows, for Orama Cloud only (lake#564, decision 56, migration `0073_billing`): billing
records live in PostgreSQL as multitenancy (`billing_plans`, `billing_plan_prices`,
`billing_invoices`, `billing_invoice_lines`) and usage stays in each workspace Zetta, read through
`usage_over_time`; an invoice freezes only the quantity and rate billed per dimension. Lake is
priced by usage only (lake#635, no migration): one price list, one rate per usage dimension, no
plans, no fee, nothing included, every account billed at it; each publication is a `billing_plans`
row, a month is billed at the list in force at its end, `billing_subscriptions` is unused, and Metis
is priced by sales, not here. Time is priced and billed per minute on the month's summed seconds
(lake#637). Nine capabilities (`price_list`, `set_price_list`, `billing_account`, `bill_preview`,
`issue_invoice`, `list_invoices`, `get_invoice`, `record_invoice_payment`, `void_invoice`), audited
centrally, are registered only where
`LAKE_BILLING=on`; off (an enterprise install), they, the Billing pages, `viewer.billing` and the
Billing guide are absent. Payment goes through the `PaymentProvider` interface. Not deployed; the
operator must vendor the schema and select `billing_guard`. See `docs/api/billing.md`.
Stripe is the payment provider (lake#620, decision 57, migration `0074_payment_provider`): lake's own
REST client through the outbound client (no SDK; form-encoded, an Idempotency-Key on every write)
makes an account a Stripe Customer at its first invoice (`billing_accounts`) and each issued invoice a
finalized Stripe Invoice with an item per lake line in cents; lake's invoice keeps the amounts, the
hosted URL (`payment_url`) and Stripe's status. `POST /billing/stripe/webhook` checks
`Stripe-Signature` (HMAC-SHA256, 300 s window) and applies `invoice.paid`, `invoice.payment_failed`
and `invoice.voided` once per event id (`billing_payment_events`), audited centrally. The keys are
lake's own platform credentials, environment variables of the hosting platform (lake#628):
`LAKE_STRIPE_SECRET_KEY` and `LAKE_STRIPE_WEBHOOK_SECRET`, read at start and never logged (Railway for
Orama's deployments; `spec.env` with a `secretKeyRef` on the dev cluster), never a user's secret
store; without them, the manual provider, said on the Billing page and in `viewer`/`server_info`. Not deployed; the operator must vendor the schema
and select `payment_provider_guard`. See `docs/api/billing.md#stripe`.
Compute is priced as CPU, RAM, GPU and GPU memory (lake#643, migration `0075_workload_resources`):
the Orama operator writes `workload_resources`, what each workload (a workspace engine, service,
store, app, image build, agent launch) is scheduled with (requests, or limits without requests;
GPUs, their class and memory); the store root's owner records `cpu`, `ram`, `gpu` (by class) and
`vram` seconds in each workspace's `lake_usage`, and the price list rates them per vCPU-, GiB- and
GPU-minute, with a GPU rate per class (`billing_plan_prices.class`). `instances`, `build_compute` and
`run_compute` stay measured but are no longer priced; issued invoices keep their lines. Not deployed;
the operator must vendor the schema, select `workload_resources_guard` and report the resources
(orama-operator#288).
See `docs/api/metering.md#resources` and `docs/api/billing.md#price-list`.
Every kind of usage is metered and billed (lake#658, migration `0077_metered_jobs`): a finished
build, job launch or agent cron run (new `workload_resources` kind `agent_cron_run`) is charged once
whenever lake first finds its resources reported, in the hour it ended, recorded `if_absent` in its
workspace (`lake_metering_job_records`); `transcribe` names its workspace (lake#613); Zetta's
`storage_usage` adds `storage` kinds `models`, `indexes`, `runs`, `streams`, `files`, `buckets`;
`search_index` counts the priced `searches`; drive, image, file and Hub bytes count as `transfer`;
`instances`, `build_compute`, `run_compute` are unbilled detail (`billed: false`).
`tests/test_usage_contract.cpp` fails on an unmetered capability, workload kind or store type, or a
billed dimension without a list rate. Not deployed; the operator must vendor the schema, select
`metered_jobs_guard` and report cron runs and job templates. See
`docs/api/metering.md#what-is-metered-where`.
Deleting and revoking follow (lake#645, migration `0076_organization_deletion`): `delete_organization`
(an owner; refused while its account has workspaces, which it names; the organization and its account
are marked `deleted_at`, never erased, because the central audit chain names the account),
`revoke_invitation`, `revoke_grant`, `delete_secret_mount` (every record out of the mount's workspace
Zetta before its route), `delete_dataset`, `delete_model`, `delete_run`, `delete_index` (the workspace
engine's deletes, zetta#1257; `not_supported` from an older engine) and `delete_ask_conversation`
(with the agent tasks the conversation started, lake#673), each with `dry_run: true` and a typed confirmation in Lake Console. Not deployed; the operator must
vendor the schema and select `organization_deletion_guard`. See `docs/api/deletion.md`.
A user's secret reaches a Kubernetes Secret only as a sealed projection (lake#625, decision 58, no
migration): lake has two levels of secrets, Orama's platform credentials as environment variables
of the hosting platform and users' workspace secrets in the secret store (`docs/deployment/secrets.md`).
`mint_sealed_secret`, for the operator credential only, answers a `LakeSealedSecret` source's fields
sealed to the cluster's X25519 delivery key in the operator's envelope version 1, never a value; an
app's secret environment names a lake secret (`lake-secret://<mount>/<name>#<field>`) and the
operator projects it, refreshing every five minutes. Not deployed; needs the orama-operator change.
See `docs/api/secrets.md#projections`.
Dynamic-secret leases follow (lake#362, migration `0058`): each lease is a sealed record in its
mount's workspace, and PostgreSQL keeps only `secret_lease_routes` (lease → mount, workspace, and
`due_at`), the content-free route and due index. Renew, revoke and the owner loop's expiry pass
rewrite a lease at the revision read, so a renewal never revives a revoked or expired lease; the
pass claims due routes and reads only the workspaces that hold them. See
`docs/api/secrets.md#dynamic-leases`.
Forge records follow (lake#341, migration `0054`): issues, reviews, comments, releases, webhooks,
exports and branch protection rules live in the workspace Zetta of their repository's workspace,
found by repository and kind; the push path reads branch protection there once before the push
and refuses reference updates when it cannot. See `docs/api/forge.md`. The web app opens, discusses,
edits, labels, assigns, closes and reopens issues through the same capabilities (lake#598, not
deployed); `create_issue` and `create_issue_comment` record the caller as `author`, and
`issue_assignees` lists the repository's people (`docs/ui/forge.md#issues`).
Application users and organizations follow (lake#353, lake#354, migration `0055`): they live in the
application's workspace Zetta, with a pool user's email and an organization's TAAP identity and name
as claims written `if_absent`; every session issued in an application's pool is checked against the
user's record there before it is returned, and the identity capabilities are asynchronous. See
`docs/api/apps.md`.

Repository environments and the platform mirror follow (lake#358, lake#359, migration `0060`): a
repository's environments and the operator rows lake mirrors for it live in the workspace Zetta of
the repository's workspace, written by the sync with conditional writes; each environment's policy
object stays in PostgreSQL under the environment's derived id, so authorization reads no workspace.
See `docs/api/platform.md`.

Agent tasks follow (lake#374, lake#375, migration `0062`): each task's request, its agent's
messages and its answer are one sealed record in the task's workspace Zetta, written before the
dispatch row and changed only after PostgreSQL accepted an event, conditionally on the revision
read. `agent_tasks` and `agent_task_status` stay as the content-free dispatch rows; the claim and
its lease remain one PostgreSQL statement, and the claim reply reads the request just before it.
See `docs/api/agent-bus.md#where-a-task-lives-lake374-lake375`.

Lake's audit splits by subject (decision 52, lake#317, lake#318, migration `0063`): control-plane
and identity events stay in the central hash-chained `audit_events`; events about a workspace's
data go to that workspace's own chain, `lake_audit_events` in its Zetta, queued on the request path
and flushed by the owner loop every second (`AuditRecorder`). `audit_route`
(`src/api/audit_routing.cpp`) classifies every registered capability; `audit`, `my_audit`,
`/activity` and `/audit` read both logs, and `verify_audit`/`export_audit` take `workspace_id`.
`audit_retention` stays central; a workspace's audit is kept with the workspace. See
`docs/api/audit.md#which-log`.

Every repository belongs to one workspace (lake#315 part 1, migration `0051`): an immutable
`repositories.workspace_id` required on every create and import, returned on reads and carried
through push authorization. It is routing only; repository metadata and content have not moved.
Repository pages keep their addresses and name the workspace. See `docs/api/repositories.md`.

The repository row is routing only (decision 54, lake#315 part 2, lake#319, migration `0064`): a
repository's description is a sealed record in `lake_repositories` of its workspace Zetta, written
`if_absent` before the row at create and read by `get_repository`, `list_repositories` (one read per
workspace) and the repository pages; the synchronous `Registry::call` answers the routing row alone.
`repository_refs` is dropped; `refs` answers from liquid alone. The `description` column stays until
the copy is verified on dev. See `docs/api/repositories.md#where-the-description-lives-lake315-part-2`.
Deleting a workspace cascades (decision 54 S12, lake#514, migration `0065`): one transaction
tombstones its repositories (live and retained) with their retention ended, deletes its apps,
cancels its unfinished builds and records the deletion in `workspace_deletions`; lake then untags
its tenant images, drops search segments and records one central audit event per destroyed object,
and the owner loop resumes a deletion left unfinished. `delete_workspace {dry_run: true}` and the
`/workspaces` confirmation list what goes, with counts. See `docs/api/workspaces.md#deletion`.
A repository left without a workspace is assigned its space's only live workspace by migration
`0066_assign_unrouted_repositories` (decision 54 S14, lake#513), through a routing-trigger exception
only that block opens; on dev this is the `liquid` repository. See
`docs/api/repositories.md#the-owning-workspace`.
Lake's process metrics and request logs stay with the instance (decision 54 S9, lake#384, no
migration): a request log line names the route pattern, the request id and the captured UUIDs,
never the raw path, so it carries no customer content; a request routed to a workspace is counted
in that workspace Zetta's `lake_requests` series (route, capability, status class) by a bounded
`RequestRecorder` the owner loop flushes every ten seconds with `record_metric`. See
`docs/serving/telemetry.md#the-workspace-request-series`.
The registry splits (decision 54 S4-S6, lake#383 phase A, migration `0068`): a tenant OCI
repository (`<space>/<name>`, `<space>/cache/<name>`) is routed by the content-free
`oci_repository_routes` (space, name, workspace; written by a build's first push of its own target
from its workspace or by `create_image_repository` with an explicit `workspace_id`; never changed;
deleted with the workspace or by the operator's `unroute_image_repository`), and its manifests and tags are sealed `lake_oci_manifests`/`lake_oci_tags` records in
that workspace's Zetta, so image addresses do not change. npm, Python, debuginfod and one-component
OCI stay with the instance. `copy_workspace_records` (`oci_tags`) moves the tags written before.
Phase B (decision 54 S5, no migration): a tenant blob uploaded into a switched workspace is an
object of that workspace's liquid store, mapped from its SHA-256 by its `lake_oci_blobs` record
(liquid addresses objects by its own hash, under encryption), and pulls stream it 4 MiB at a time;
blobs pushed before stay in the instance registry, still served from there, until
`copy_workspace_records` (`oci_blobs`) copies and verifies them; lake removes nothing from the
instance registry. See `docs/api/registry.md#where-a-tenant-image-lives`.
The operator gets its volumes (decision 54 S13, operator#259, operator#260, migration `0069`):
`workspaces.storage_gi` (default 5, never decreasing, refused by a trigger) is the desired size of
each workspace Zetta volume, with the operator's observed size, phase and message on the row;
content-free `volume_snapshot_policies` (target `store` or a workspace id, cron schedule, keep,
`requested_at`) and the operator-written `volume_snapshot_status`. `set_workspace_storage`,
`request_volume_snapshot` and `volume_snapshots` are operator or instance-administrator only and
audited centrally; the `move_workspace_store` snapshot check waits for lake#380's change. See
`docs/api/volumes.md`.

A workspace's liquid store moves into its workspace Zetta (decision 54 S1-S3, lake#380, migration
`0067`): `ZettaObjectBackend` is liquid's `store_backend` over Zetta's object routes (bucket
`lake_store`), encrypting every byte below liquid with the workspace's data key (wrapped under a key
derived from `LAKE_INSTANCE_KEK`; fixed AES-256-GCM blocks plus a replaced tail object, so appends
never rewrite); `ZettaStoreOwner` is liquid's `store_owner` over the bucket lease, renewed every third
of `LAKE_WORKSPACE_STORE_LEASE_SECONDS`, and a lost lease answers that workspace `503
store_unavailable`. Workspace stores open on first use and close on drain and on workspace deletion.
The operator's `move_workspace_store` plans, copies (liquid#328), verifies and switches one workspace
at a time (`workspaces.store_moved_at`); the space store is never written or removed. A switched
workspace's missing store, key or repository answers `corrupt`, never an empty store; each store has
its own lane with at most four requests admitted, a moving store answers 503 with Retry-After, and
repository-store work (search included, which reads stores only through the repository store)
runs off the server thread, so one workspace's Zetta holds up only its own requests. Rollback by the workspace Zetta is not detected yet (lake#522), and
every workspace store depends on `LAKE_INSTANCE_KEK`, which has no rotation or backup. Replication
cursors go through liquid's cursor API. It needs liquid `8955519` or later (liquid#332, write-once index
runs), and `88c5ef4` or later for the fan-out of held puts: before it, corosio read 8 bytes before a
continuation that a range `when_all` posted and could fault (lake#604, corosio#13), and an offload's
completion could signal a `block_on` context that had already ended (liquid#350); no workspace is switched on dev, and the copy's snapshot
precondition is not enforced (the move does not wait on the `0069` snapshot state yet). See `docs/architecture/workspace-store.md` and
`docs/api/workspace-store.md`.
A switched workspace's search index lives beside its store (decision 54 S8, lake#381, no migration):
the manifest, write-once blob runs, segments and artifact members are `search/` objects in bucket
`lake_store`, sealed with the workspace's data key and fenced by the store's lease through search's
own client calls (a failed index write never fails the store), written under the store's lane and
within the room the space's quota leaves after a tenth kept for the repositories. Loaded on first search, evicted when idle under
`LAKE_SEARCH_MEMORY_BYTES`; a missing or unreadable index is rebuilt on search's own worker thread
(releases from their records), never inside a request and never copied, and answers
`building_workspaces` meanwhile. `search` decides the caller's spaces and workspaces before any
workspace is opened. A switch and every start drop switched workspaces from the space index on
lake's volume; deleting a repository or a workspace removes it from the index. `search` gains an
optional `workspace_id`; the interim skip of switched workspaces is gone. A failed index write leaves
its repository pending, retried by search's worker after 15 s doubling to 10 min, and search names
the workspace in `stale_workspaces` until then (lake#540). See `docs/api/search.md`.

## Historical status — superseded 2026-09-08

The following was the prior source-tree status record. Its production claims are retained only as
historical context and are superseded by the dated deployment record above.

**The control surface serves, in production.** `Lake/prod` on AKS (3 replicas, image
`oramasearch/lake:20260902c`) runs: one capability registry rendered as REST `/v1`, OpenAPI
(`/v1/openapi.json`) and MCP (`POST /mcp`); users, organizations, customers and repositories over
the Postgres control plane (schema `0001` + `0002` applied); server-rendered pages for customers
and repositories behind the admin sign-in; liveness, readiness,
Prometheus metrics and JSON request logs. Capabilities and metrics use the admin bearer except
`server_info` and documented public identity and transparency-monitoring reads; probes are
unauthenticated. State and runbook: `docs/deployment/aks.md`; the specification
is `oramasearch/liquid/docs/features/`. Next: the Zetta data plane (operator #125), repositories with content
once liquid Phase 1 lands, incremental migrations (operator #126). The codebase also includes
organization administration, one policy engine, migration `0004`, and settings pages for organizations,
teams, members, invitations, grants and effective access; those are not yet deployed in the
production state above. It also includes the signed, hash-linked audit stream (one event per
capability call, sign-in and session event, recorded by `Registry::call` and the session routes),
the lake TAAP identity with rotation, audit verification and export, the 400-day retention
floor, migration `0005`, and the `/audit` page; `LAKE_INSTANCE_KEK` is required wherever a
Postgres store runs. Repository lifecycle adds migration `0006_repository_lifecycle`, repository
rename, transfer, visibility and tombstone capabilities, and lifecycle state on the repository
page. Those capabilities use the policy metadata and are recorded by the registry audit stream.
Platform support links a client-only `zetta-api` from the required `LAKE_ZETTA_SOURCE_DIR`, including
the native transport's runtime event implementation and sharing Lake's existing Boost.HTTP/Beast2
stack. The workspace store does not use Zetta's per-request client: `lake-object-http` keeps
connections to each workspace Zetta and resolves its name once (lake#544; on dev each resolution
cost about 18 ms of a 20 ms request). It mirrors operator-control deployment, environment, and
domain rows into migration `0019_platform`, creates environment policy objects, exposes the eight
policy-checked platform capabilities, Loki-compatible logs, and the server-rendered environment
pages. Operator-control outages degrade platform health, preserve stale mirror reads, and refuse
platform writes without changing readiness.
The codebase now renders forge-designed repository homes, trees, highlighted files with line
anchors and blame, change review with safe Markdown, comments, diff layouts and checks, issues,
releases, webhook history and retry, branch protection with signed refusal audit, explained conflict
values with audited resolution,
paged artifact results with verified attestations and trace summaries, and effective access. The
Postgres-backed forge scenario proves an unlanded Liquid review, cursor movement after rewrite,
approval, publication, and graph-derived evidence without repository automation YAML.
Shared empty, pending, and error states retain the design-system shell and keyboard order;
anonymous readers receive only public repositories. These repository data pages are not part of
the production state above.
Lake links libliquid as a library-only submodule and opens one customer store at
`LAKE_STORE_ROOT/<customer id>/store`; liquid's client, server, sync, per-reference outcome, and
transparency verifier contracts are pinned. The source tree serves repository
push, pull,
missing-object negotiation, chunk upload,
and ranged reads through the registry, with per-change and protected-reference policy and signed
audit. These routes are not
part of the production state above until a containing image and operator volume support are
deployed. The source tree also provides signed portable repository export/import, including the
liquid history/object transfer, forge state, direct grants, artifact evidence, optional Git
projection, and its export download page. Recorded GitHub imports map history, issue/review
discussion, and workflow documents with a durable fidelity report. Identity core
adds migrations `0007_identity_core` and `0008_mfa_recovery`, verified TAAP documents, password
plus TOTP with one-use recovery codes, persisted WebAuthn, OAuth, row-resolved sessions, scoped
tokens, device revocation, lake-signed passkey-to-TAAP bindings, and the hosted identity pages.
Migration `0021_token_attenuation` bounds issued tokens by issuer scopes and expiry, restricts
static derivation to existing static authority, and invalidates descendants after ancestor or
device revocation. It revokes pre-migration API/static tokens once because ancestry is unavailable.
Migration `0022_session_lookup` selects PostgreSQL sessions by a unique public UUID and verifies
a SHA-256 digest of a 256-bit random bearer. Operator credentials are compared first in constant
time. Legacy sessions without selectors are revoked once and require sign-in and token replacement;
the operator must vendor and apply this schema before deployment. See `docs/api/auth.md`.
Identity documents are verified through libliquid's public canonical-CBOR codec. Lake-pool password
sign-up atomically provisions a personal customer and its inherited ownership grant, then directs
the user to a first-repository form. Generated handles, default quotas, and zero-byte metering are
documented in `docs/api/provisioning.md`.
The source-tree landing page describes hosted Liquid repositories and the existing account,
private-repository, and account-token push path. Shared navigation receives the resolved viewer:
public repository browsing for visitors, personal workspace and account links for users, and
Administration for operator credentials. Customer authentication redirects use `/sign-in`;
`/login` remains an explicit operator entry. See `docs/ui/front-door.md`.

Migration `0020_identity_exit` adds fresh, replay-resistant libliquid TAAP-key sign-in and a
one-use application authorization-code flow with exact redirects, PKCE S256, nonce-bound ID
tokens, and the existing Ed25519 application JWKS.
SSO federation adds `0010_sso_federation`, organization SAML/OIDC provider configuration,
federation issuer-claim policy, SCIM user/group mappings, deactivation session revocation,
provisioning logs, the `/sso` page, and provider-bearer SCIM 2.0 `/scim/v2/Users` and `/Groups`
resources. The operator copies the schema once per wave in its own operator pull request.
Offline authorization and account recovery add `0011_offline_recovery`: lake-signed device-bound
cached-read grants with a seven-day maximum, per-change publication checks against grant revocation
and current policy, one-use recovery codes, email recovery with a 24-hour cooling period,
two-administrator organization recovery, per-device notices, and designated custody of opaque
client-wrapped repository trust-domain keys. Account recovery restores sign-in but cannot recreate
a repository key held only on a lost device. The full contracts are `docs/api/offline.md` and
`docs/api/recovery.md`.
Repository search persists blob-deduplicated BM25F and positional-trigram segments at
`LAKE_STORE_ROOT/<customer>/index/`. It serves hot branch tips, immutable release and tag
snapshots, exact changes, and complete history; reports `hot`, `snapshot`, or `scan` mode;
atomically publishes through its manifest; filters candidates with batched policy authorization;
and records the signed query outcome through `Registry::call`. REST, OpenAPI, MCP, and the
`/search` page expose the same capability.
Published artifact outputs are decomposed through libliquid's format registry after sync
publication or through the audited `reindex_artifacts` action. Scoped artifact search reports
indexed text and source members with artifact identity, member path, digest, and line; binary
members and containers nested beyond one inner level remain listed by digest only.
Metering migration `0014_metering` stored hourly unique-byte gauges, sync transfer counters, and
idempotent build-compute counters; since `0053` (lake#348, lake#349) transfer and build records live
in each repository's workspace Zetta and storage is measured live. `metering_usage` and `/metering` expose configured integer
pricing, with audited per-customer quota overrides. Its current summary also measures registry
image bytes, workspace Zetta dataset bytes, image build time and workspace run time on each read,
without storing them, and lists what is not measured yet (lake#444). `LAKE_STORAGE_QUOTA_BYTES` remains the
default and throttles new writes at the customer-store boundary while existing content remains
complete and readable. The contract is `docs/api/metering.md`.
Static secrets and encryption add migration `0009_secrets`: customer-namespaced organization
mounts; immutable JSON versions with soft delete, restore and destroy; named keys with rotation,
encrypt/decrypt/rewrap, Ed25519 signing, HMAC-SHA-256 and stored child derivation; and customer-KEK
rotation that rewraps data keys without rewriting secret ciphertext. The mount, secret and key
pages render metadata only. Every operation uses policy authorization and the signed registry
audit path, including denials. Renewable and revocable dynamic-secret leases encrypt the generated
credential and mirror their signed lifecycle audit rows into the transparency log.
Customer PKI adds persisted private roots and online intermediates, SPIFFE X.509 and JWT SVIDs,
client, device, OpenSSH user and OpenSSH host certificates, and a customer-scoped RFC 8555 ACME
HTTP-01 flow. Migration `0013_pki` holds the durable state. ACME consumes one-use nonces, verifies
ES256 account JWS and the final CSR, and runs every resource operation through its registry
transport for signed audit. Agent TAAP Ed25519 public keys receive short-lived X.509 certificates;
rotation links the replacement, revocation is explicit, and API checks reject revoked or expired
rows.
Caller exchanges persist verified non-secret identity metadata before returning a bearer
credential, and OIDC/JWT claims resolve through `federation_issuers`. Root-recovery ceremonies
persist quorum, expiry, completion, and one-way share evidence while keeping shares and
reconstructed root material out of storage. Both use the signed registry audit path.
Drive and models add migration `0015_drive`, stable-node folder ACLs, live recipient grants,
expiring and revocable links, list/grid browse and repository mutations, browser-native and
server-rendered format-registry previews, version comparison, explained conflict presentation and
resolution, per-path
history and restore, effective access before sharing, link management, and shared-with-me pages.
Bearer-link recipients can traverse descendants, preview files and download exact bytes through
public registry capabilities that check link scope, role, expiry and revocation on every call.
The pinned Liquid drive client is covered end to end against an in-process Lake and Postgres:
regular-user sign-up, synchronization, sharing, rendered history, disconnected conflict
preservation, repository pull and materialization, and a native project build.
Models and datasets use
the liquid sync protocol for resumable deduplicated transfer and unique-byte metering. The
compatible Hub routes support create,
file/folder upload, basic LFS, list, info, snapshot, and named or digest downloads. Model metadata
and the server-rendered model page include per-revision storage savings plus the verified liquid
attestation chain from dataset and code revisions through the training run. Linux desktop
presentation remains tracked in liquid #196.
TUF repository metadata adds immutable root, targets, snapshot, and timestamp objects at
`LAKE_STORE_ROOT/<instance>/log/tuf/`, distinct-key threshold verification, version/expiry and
cross-role binding checks, dual-threshold root rotation, and a rehearsed compromised-key
replacement. Its four capabilities use policy metadata and the signed registry audit stream.
Transparency checkpoints carry signed UTC Unix-second `issued_at` values and refuse clock rollback
across publishers and restarts. Checkpoints read `LAKE_LOG_MODE`, witness endpoints and public keys, quorum, and an
optional aggregation endpoint at startup. Air-gapped checkpoints make no outbound call; witnessed
and anchored checkpoints require the configured distinct witness quorum before they become current;
anchored checkpoints retain an aggregation proof. The effective configuration is a read capability
and appears on `/audit`; the Lake-CR environment mapping is `docs/control-plane/log.md`.
Canonical TAAP attestations append to immutable 1,024-entry and interior-hash tiles, and checkpoints
bind their Merkle root and tree size. Public tile and compact inclusion/consistency-proof
capabilities bind responses to exact signed checkpoints. The hosted `/transparency` identity
monitor verifies the complete checkpoint root before returning matches and refuses modified tiles.
The pinned liquid #207 verifier checks the identity-exit dynamic-secret inclusion proof end to end.
The proof-bundle capability exports all checkpoint trust material and tiles for liquid's offline
verifier. Liquid's persistent checkpoint trust rejects incompatible equal-size witness views.
The source tree also serves OCI Distribution, npm, Python package, and debuginfod protocols from
instance-local registry storage. Protected publication and public exact-byte reads are documented
in `docs/api/registry.md`.
Customer repositories and the transparency log replicate asynchronously from one active Lake to
standbys. Repository pulls persist per-source liquid cursors after successful import. Log pulls
require the destination's exact Merkle prefix, verify the proposed head and signed checkpoint, and
never roll back. `docs/api/replication.md` defines consistency;
`docs/control-plane/replication.md` is the operator handoff.

RFC 3161 timestamping hashes checkpoint signature values, verifies TSA responses and trust, and
stores DER RFC 4998 evidence records at `LAKE_STORE_ROOT/<instance>/log/timestamps/`. The owner
loop renews records by timestamping the preceding archive timestamp before the configured age or
TSA certificate-expiry margin. Creation, renewal, and reads are registry capabilities on the
signed audit path.

Application authentication adds migration `0016_auth_apps`, isolated identity pools, themed
hosted and custom-domain entry pages, per-application Ed25519 JWTs and JWKS, signed lifecycle
webhooks with retained attempts, and authorized operator management for users and organizations.
Its contract is `docs/api/apps.md`.

Ship Apps add migrations `0034_web_apps` and `0035_web_apps_operator_contract`, customer-scoped
REST and MCP capabilities for create, deploy, rollback, lifecycle, scaling, environment, service
binding, Loki logs, and app-specific workspace
events. The `apps` row uses runtime/image modes and is shared with the Orama operator, which owns
app tokens and writes only their hashes into Lake. Lake validates purpose and target, and removes
rows after the operator acknowledges deletion. This App entity is distinct from application
identity pools. Its contracts are `docs/api/web-apps.md` and
`docs/control-plane/web-apps.md`.
Protected Ship apps use `GET /v1/apps/auth` (also under `/w/<workspace_id>`) for nginx
forward-auth and `/apps/authorize` for the Lake-session handoff. The app cookie is host-only,
expires after 12 hours, and rechecks workspace membership on every request. Migration
`0041_app_forward_auth` stores only short-lived one-use code hashes.
The Ship → Apps pages use those capabilities for creation and all controls, mask Secret references,
follow retained app/build events, and start Application Agent app turns with exact proposal
confirmation before repository or App writes. Their contract is `docs/ui/apps.md`.

In-cluster image builds add migration `0036_builds`, customer-scoped REST/MCP capabilities,
queued/cancelled desired rows, operator status, build-specific source/push/event hashes and retained
Zetta progress. Build targets use the in-cluster Lake Service and a customer-handle registry
prefix. The contracts are `docs/api/builds.md` and `docs/control-plane/builds.md`.

Train → Streams adds nine space-scoped REST/MCP capabilities for topic/message CRUD and
non-consuming live browse across workspace and bus Zettas. System topics are browse-only;
topic settings/drop require recorded creator ownership. Since lake#379 each creator is a sealed
record in the topic's workspace Zetta (`lake_stream_owners`), so ownership follows the workspace
incarnation; migration `0047_stream_owner_workspaces` records the retired `stream_owners` rows'
workspaces for the operator's copy. Lists are cursor-paged from one `list_topics` call per Zetta;
workspace-native event topics and the space's Agent tasks rows replace per-turn virtual rows. See
`docs/api/streams.md`, `docs/ui/streams.md` and `docs/control-plane/streams.md`; the operator
schema/guard handoff remains a rollout prerequisite.

Workspace data-plane cutover adds migrations `0038_workspace_data_plane` and `0039_workspaces`:
tasks, apps and builds retain immutable explicit `workspaces.id` values; library-agent bus and task
credential calls may omit the ID because Lake derives it from the task and rejects a mismatch.
Event topics use that workspace, never the Lake bus. Only agent dispatch topics remain on the bus.
Design, Train and Ship navigation uses
`/w/<workspace_id>/...` and a global top-bar selector. The storage audit and remaining migrations
are tracked in `docs/architecture/workspace-data-plane.md`; operator cutover and upstream
60-second polls are linked follow-ups rather than deployment claims. Workspace CRUD
and the Workspaces page write desired state; the operator owns observations. See
`docs/control-plane/workspaces.md` for the contract, guard and approved dev-cluster replacement.

Agent-session custody adds migration `0017_agent_sessions` and a maximum 15-minute Ed25519 key
delegated to a durable agent by a canonical `delegation/1` attestation. The attestation is appended
to the tiled transparency log before public session metadata is persisted; the private seed is
returned once and never stored. Drip verification binds the session signature back to the durable
agent and rejects the key at expiry. Its contract is `docs/api/agents.md`.

## Creating users

To give someone a lake account in any environment (Clerk on lake.oramadev.net, lake.live, local,
on-prem) or take access away, follow [`docs/deployment/users.md`](docs/deployment/users.md). On
dev, accounts are added in the Clerk application `lake`, never through the site.

## Testing in a browser

Sign in to lake.oramadev.net from a browser, and walk every page, with a one-time Clerk sign-in
ticket: [`docs/deployment/browser-testing.md`](docs/deployment/browser-testing.md)
(`tools/e2e/clerk-ticket.sh`, then `tools/e2e/walkthrough.mjs`). Do it after any roll that changes
what a person sees, and look at the screenshots before calling a UI change done.

## Where to read Zetta

lake builds on Zetta, so you will read Zetta constantly. **Read it only from the umbrella
bridge:**

```
/Users/stevesperandeo/dev/homer6/orama-work/modules/zetta-bridge/modules/zetta
```

Never from a standalone clone — one under `~/dev/oramasearch/zetta` was 1,053 commits
behind and lacked the entire HTTP layer lake borrows. The web-serving stack lake copies is
`include/zetta/api/server.hpp` + `src/core/api/http_server.cpp` (Beast2 on capy/corosio,
one adapter TU) and the pure renderers in `include/zetta/api/web_ui.hpp`.

## How the server is built (and the rules that keep it Zetta-shaped)

- `include/lake/api/server.hpp` exposes `ApiServer` / `ServerOptions` and **no HTTP
  type** — no beast2, `boost::http` or Boost.JSON name may appear in a public header.
- `src/api/http_server.cpp` is the **one** adapter TU. Handlers are `Impl` members
  returning `boost::http::route_task`; every response goes through `send_text`, which
  stamps headers Zetta's way (`erase(etag)`, `erase(content_length)`,
  `set_payload_size`) — Beast2 frames from the declared size, never set those by hand.
- Routes are registered in `ApiServer::start()`: `router.use(http::cors())`, the
  `router.add(...)` list, then a **trailing `router.use` 404 handler**. The router falls
  through (`route_next`) past the last route and nothing answers otherwise — without the
  trailing handler an unmatched request hangs. Keep it last.
- The first router middleware clears the entire pooled response before CORS, then restores the
  current request's HTTP version and keep-alive policy. Every route starts without another
  request's headers; never move this reset into a send helper after handlers have added fields.
- Lifecycle is Zetta's: construct `beast2::http_server(io_context, workers, router,
  parser_cfg, serializer_cfg)` → `bind` → `local_endpoint().port()` → `start()` →
  `io_thread` runs `io_context.run()`; `stop()` = `server->stop()`, `io_context.stop()`,
  join. The owner loop calls `wait_for_work()` then `drain()` each tick; both are
  placeholders until lake has a node to marshal onto.
- **The console is the web app** in `frontend/` (React, Vite, Tailwind, TypeScript), built and
  embedded by CMake and served by the `web_app` middleware: a browser's GET of any page path gets
  its `index.html` (`Content-Security-Policy: frame-ancestors 'self'`), anything else the route or
  the 404. It calls **only `/v1` capabilities**, so it can never do what the API cannot; do not add
  a server page or an HTML form handler. Build, page rules and checks: `docs/ui/web-app.md`. The
  server routes the app relies on (the cookie-setting sign-in routes and the download routes) are
  listed there and in `docs/serving/http.md`.
- **Hosted application pages and the region picker are the server-rendered surfaces**: an identity
  application's sign-in pages (`/apps/<id>/*` and its custom sign-in host), rendered by the pure
  functions in `web_ui.cpp` in the application's colours, with the theme scripts and, on the passkey
  page, one script for `navigator.credentials`; and the region picker every path of
  `LAKE_APEX_HOST` answers (`web_ui_region_picker`, lake#757, `docs/api/regions.md`). They work
  without JavaScript otherwise.
- **Nav items and headings are Title Case** — "Search Indexes", not "Search indexes": every
  rail item and section label, page and section title, and `<h1>`–`<h4>` in the web app and the
  hosted pages.
  Capitalize every word except articles, coordinating conjunctions and prepositions of four
  letters or fewer (*a, an, the, and, but, or, for, as, at, by, in, of, on, to, via, with, from,
  into, over*), which stay lowercase unless first or last; acronyms stay as written ("API Tokens");
  phrasal verbs keep their particle ("Sign In"). Body text, buttons, form labels and eyebrows stay
  sentence case; a heading written as a full sentence ending in punctuation stays a sentence. Rules
  and examples: `docs/ui/design-system.md` → *Capitalization*.
- IBM Plex Sans and Mono are self-hosted WOFF2 assets: the web app bundles them under `/ui/`, and
  the binary also serves them below content-hashed `/fonts/` URLs for the hosted pages, which link
  the content-hashed stylesheet; unhashed stylesheet and font aliases use `no-cache`. The browser
  never needs a third-party font origin. Both use the same Orama design tokens
  (`tools/ui/build-tokens.mjs`, `docs/ui/design-system.md`).
- Static visual-direction references live under `docs/design/ui/mocks/`;
  `tools/ui/screenshot.sh` renders them at 1440×900.
- **One auth check.** The API bearer and the UI session cookie (`lake_session`, set by
  `POST /session`) carry the same secret and are resolved by the same code (`context_for`);
  never add a second path. The web app guards its own pages from the `viewer` read; a protected
  server route that remains (a download) redirects to `/sign-in` without a valid credential;
  `/metrics` uses the same check. `/healthz` and `/readyz` are deliberately unauthenticated Kubernetes probes.
- **Cross-site requests.** An unsafe request the `lake_session` cookie authenticates must come from
  lake's own origin (`Origin` and `Sec-Fetch-Site`), and a cookie-authenticated `/v1` or `/mcp`
  `POST` must be `application/json`; bearer calls are unaffected (`refuse_cross_site`,
  `docs/serving/http.md#cross-site-requests`, lake#561). Tests that post with a cookie send the
  headers a browser would.
- **One registry.** Every capability is registered from `src/api/capabilities.cpp` or its
  area-specific registration file; REST,
  OpenAPI and MCP are renderings of it and never special-case an entry. `Registry::call` is the
  only path that authorizes and validates. Recipe: `docs/api/README.md`.
- **Two TUs name third parties:** `src/api/http_server.cpp` (Beast2, boost::http, Boost.JSON)
  and `src/control/postgres_store.cpp` (libpqxx). nlohmann::json is the API document type.
- Operational telemetry: `GET /healthz`, `GET /readyz`, authenticated `GET /metrics`, and one JSON
  request log on stdout. Contract and metric names: `docs/serving/telemetry.md`.
- Idle keep-alive closes are normal and produce no stderr output. Lake pins the
  `oramasearch/beast2` `orama-main` carrier branch that suppresses its former end-of-stream line.

## What lake is built on

- **Control plane: Postgres, for now.** The operator provisions a CloudNativePG cluster per
  `Lake` (`spec.postgres`, on by default) and injects `LAKE_CONTROL_DB_URI` /
  `LAKE_CONTROL_DB_PASSWORD` into the pod (optional refs: the pod starts before the DB is
  bootstrapped, so treat an empty value as "not yet"), and `LAKE_ADMIN_TOKEN` from the
  minted-once `lake-<name>-admin` Secret. **Data plane: Zetta.** Recorded in the
  bridge `decisions.md` §6; "for now" is deliberate.
- **Schema.** `db/schema.sql` is the source of truth; the operator carries a byte-identical
  vendored copy at `internal/controller/assets/lake-schema.sql` and applies it per Lake as an
  owned `PostgresSchema` (one transaction, guarded on the table the **newest** migration creates,
  `network_guard` in the source tree, versioned by content hash).
  Change the schema HERE, then copy it into the operator in the same change; the operator's test
  checks the copy carries the source-of-truth header. Migrations are single-shot today
  (incremental migrations are the PostgresSchema follow-up), so until then a schema change means
  a new guard (the operator's `GuardTable`), idempotent SQL (`IF NOT EXISTS`), and on an existing
  Lake deleting its `PostgresSchema` so the owned object is recreated and the Job re-runs. Migrations never run in lake pods, so
  replica count cannot duplicate them.
- **Tenant model.** user and organization are identity (TAAP `taap_id`); the customer owns
  everything: individual xor organization (`customer_type_check`), one customer per identity,
  one global lowercase handle namespace (`customer_name_unique` + `customer_name_format`); the
  app generates handles and must pre-check to answer 409.

**lake is the enterprise product and builds on Zetta as much as possible** — control
plane, operational state, clustering, and **its web-serving approach**. lake must also
host liquid repositories, so it consumes **liquid's store as a library** for repository
data. liquid is open source and never depends on Zetta; lake sits on the other side of
that licence boundary.

- **Port 4900** (decided; Zetta is 4800). The `Lake` CR's `spec.port` defaults to it.
- **C++26**, CMake ≥ 3.28 + Ninja, Clang 17+ auto-selected as Zetta's `CMakeLists.txt`
  does, OpenSSL 3 and libpq from Homebrew. GoogleTest v1.17.0 via FetchContent (as Zetta), not Catch2.
- **libliquid** as a library-only submodule supplies Capy, Corosio and nlohmann/json;
  **Boost.HTTP** and **Beast2** provide the HTTP/1 server. `http`, `beast2` and `libpqxx`
  remain vendored under `modules/`. Bump liquid only to its current `main`; never edit it
  through lake's copy.
- `std::expected` at every public boundary; no exceptions across public APIs.
- Deployed as a `Lake` CR on `oramasearch/orama-operator` (the Zetta operator, extended);
  runs on `oramasearch/k0s`, and on our AKS cluster for now.

## Container image

- `Dockerfile` = Zetta's headless recipe (Ubuntu 24.04, clang-19 from apt.llvm.org, CMake 3.31
  because 24.04's 3.28 does not know `cxx_std_26`) minus GUI/Postgres deps, **plus
  `libstdc++-14-dev`**: 24.04 defaults to libstdc++ 13, which has no `<print>`, and lake uses
  `std::println`, and `libpq-dev` for libpqxx. A checksum-verified OpenSSL 3.6.3 source build
  provides Corosio TLS and Argon2id. The runtime carries those OpenSSL shared libraries plus
  `libstdc++6 libpq5 ca-certificates`, numeric non-root `USER 10001:10001` (a named user cannot
  satisfy a `runAsNonRoot` pod security context).
- **No CI.** Steve has said no to CI (umbrella `CLAUDE.md`). Never add, enable or extend a
  workflow, never edit `.github/workflows/` to make CI pass, and never wait on or cite CI checks.
  Proof is the local suite plus the cluster BuildKit Linux lane
  (`lake-linux-test/run-lake-linux-tests.sh`, liquid-dev skill). `docs/build/ci.md` describes the
  workflow files that remain in the repository.
- Deployment images are built on the cluster BuildKit (liquid-dev skill) as immutable
  `oramasearch/lake:<tag>` images, never `latest`. The Docker Hub repo is private; the `Lake` CR
  needs `spec.pullSecretName` and the Secret in `spec.namespace`.

## Branches: feature branches and pull requests always target `dev`

Steve, 2026-10-12: *"shouldn't you be merging into dev branch? we're not cutting a release yet"*, and
*"feature branches / prs always target dev branch"*.

- **Branch from `origin/dev`**, never from `main`.
- **Open every pull request with `--base dev`**, written out. This repository's default branch on
  GitHub is `main`, so `gh pr create` without `--base` targets `main`.
- **Before merging, read the base back**: `gh pr view <n> --json baseRefName` must say `dev`.
- **Never push, merge or fast-forward to `main`** in ordinary work. `main` is the last release and
  moves only by the release pull request that promotes `dev` to `main`, when Steve names a version
  (*Cutting a release* below).
- Images built for the dev cluster come from `dev`.

The org-wide rule is in the umbrella bridge's `CLAUDE.md`
(https://github.com/oramasearch/orama-bridge/blob/main/CLAUDE.md).

## Cutting a release

A release is a tag on `main`, which is what the deployment runs. The pages and their conventions
are in [`docs/releases/`](docs/releases/README.md); read its `README.md` first. `v0.1.0`
(2026-10-12) was the first.

1. **Confirm what is deployed is `main`.** `GET /v1/server_info` reports the running `revision`;
   it must be an ancestor of `origin/main`. Steve names the version.
2. **On a branch `docs/release-vX.Y.Z`**, move the version in all three places: `version_string()`
   in `src/version.cpp`, `current_version()` in `include/lake/version.hpp` and
   `project(lake VERSION ...)` in `CMakeLists.txt`. Tests read the version from
   `lake::version_string()`; never write the number into a test. `ReleaseVersion.*`
   (`tests/test_release_version.cpp`) fails when the three disagree or when the version has no
   page in `docs/releases/` indexed in its `README.md`.
3. **Write `docs/releases/vX.Y.Z.md`** and add it to `docs/releases/README.md`: what the release
   includes, what it does not, known issues with their tracking issues (a security defect first),
   and verification. Claims follow `docs/design/status.md`; every number is measured.
4. **Prove it**: build, then the full suite with `LAKE_TEST_DB_URI` set, and put the counts in the
   page. A failure is fixed or listed as a known issue with an issue filed, never left out.
5. **Open the pull request into `main`** and merge it with a merge commit, never a squash.
6. **Tag the merge commit** `vX.Y.Z` (`git tag vX.Y.Z <merge commit> && git push origin vX.Y.Z`),
   then publish the GitHub release from the page:
   `gh release create vX.Y.Z --repo oramasearch/lake --verify-tag --title "Release vX.Y.Z" --notes-file <page without its first heading, relative links made absolute at the tag>`.

7. **Build the image from the tag and roll `Lake/prod`.** Confirm the schema did not change since
   the deployed image (`git diff <deployed revision> vX.Y.Z -- db/` is empty); if it did, the
   operator is re-vendored and rolled first. Move `wt/lake/deploy-next` to the tag and run
   `build/scripts/build-lake.sh <YYYYMMDD><letter>`, which rolls only on a successful build. The
   image is built against the liquid commit liquid-bridge pins, so move that pin to liquid's
   release first when one was cut. The commands are in liquid-bridge's `liquid-dev` skill.
8. **Check the live service**: `GET /v1/server_info` reports the release's `version` and the tag's
   `revision`, `/readyz` answers 200, and a browser's request for `/` (`Accept: text/html`) answers
   200. A bare `curl` of `/` answers 404 by design.
9. **Tag the image `vX.Y.Z` in Docker Hub**, naming the digest that is deployed, without
   rebuilding: `retag-image.py lake <YYYYMMDD><letter> vX.Y.Z` (in the `liquid-dev` skill). Both
   tags must print the digest the running pod reports, and an anonymous request for the
   repository must still answer 404.
10. **Move the pins and record the roll**: liquid-bridge's pin of `modules/lake` to the tag, the
    umbrella's pointer to liquid-bridge, and the deployment paragraph in liquid-bridge's
    `docs/action-plan.md`. Update **Status** above and the README's deployment line.

A release is finished when the Git tag, the GitHub release and the image tag all exist and the
cluster runs that image. `v0.1.0` is `oramasearch/lake:20261012a`, also tagged `v0.1.0`.

## Open decisions

- **Blocking store calls run inline on the io thread** (one statement each). Offload to a
  pool, as Zetta does, when latency shows; not before.
- **Broader Zetta linkage.** The operator-control and Loki HTTP clients link the client-only
  transport from the authorized bridge source path. Any further Zetta target remains a separate
  decision and must preserve one copy of the networking stack.

## Never

- Never run schema migrations from lake pods (the operator's PostgresSchema Job does, once).
- Never add an auth path the API does not have; the UI session is the API bearer.
- Never add a second attestation or signing framework. TAAP/1 is the security model.
- Never make this repository public.
- Never let an HTTP type leak out of `src/api/http_server.cpp`.

## Before finishing any change

1. **Document the change in `docs/<topic>/*.md`** (see `docs/README.md`): what, why, how to
   verify, how to change. New topic → new folder + a row in `docs/README.md`. This is a must.
2. `README.md` is the front door only (what it is, build and run, pointers); never append status or
   feature sections to it. Update `docs/design/status.md` for the status of your area and
   `docs/README.md` for any new page.
3. Does this file's **Status** and **Open decisions** still hold?
4. `cmake --build build && ctest --test-dir build` green, and the binary smoke-tested by hand.
5. Commit means push.

PostgreSQL tests use `lake::test::postgres_uri()` for a fresh database per test. The configured
`LAKE_TEST_DB_URI` database belongs to the caller and is not modified. Query specific audit calls
by request ID; use cursor exhaustion for whole-history assertions. See `docs/build/testing.md`.

## Formatting

`.clang-format` at the root is the formatter configuration (LLVM base, 100 columns, 4-space indents,
one statement per line). Format only the lines you change (`git clang-format`) before each commit;
existing code is not reformatted wholesale. On macOS the Homebrew binary is
`/opt/homebrew/opt/llvm/bin/clang-format`; on Linux install `clang-format-19`.
