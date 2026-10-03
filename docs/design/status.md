# Implementation and deployment status

Status: checked on 2026-10-02 against `Lake/prod` on the dev AKS cluster (lake `36679e1`), using
the [2026-10-01 deployment record](../deployment/dev-2026-10-01.md) and the OpenAPI observation
[`openapi-2026-10-02.json`](../deployment/observed/openapi-2026-10-02.json). There is no production
environment yet: `Lake/prod` and lake.oramadev.net are the development deployment.

## Status vocabulary

| Level | Meaning |
|---|---|
| **Implemented** | The source tree contains the capability or surface described by its contract page. |
| **Exercised locally** | A local test or scenario has run the implementation, including the PostgreSQL-backed cases where marked. |
| **Deployed** | A dated live observation establishes that the deployed artifact exposes the named public surface. It does not establish full behavior. |
| **Exercised in deployment** | A dated live request or scenario completed against the deployed service. Only the actions named in the deployment record have this level. |

An OpenAPI route proves that a public route is deployed. It is not an end-to-end proof of the
capability behind that route. Contracts below remain the authoritative behavior descriptions.
The *Capabilities* column names the registry operations a row speaks for; the
[check](#keeping-this-page-true) compares them with the observed OpenAPI.

## Observed deployment

`Lake/prod` runs `oramasearch/lake:20261001d`, source revision `36679e1`, as `GET /v1/server_info`
reports, with Postgres, a bound repository-store volume, workspaces enabled and the operator's
schema applied (`schemaVersion sha256:15e6a3951dfb`). Its OpenAPI lists 352 capabilities. The live
end-to-end runs of [orama-bridge#136](https://github.com/oramasearch/orama-bridge/issues/136) (Apps, Solutions S-1, Experiments) and the UX review of
2026-10-01 (liquid-bridge `docs/reviews/lake-ux/2026-10-01.md`) are the exercised-in-deployment evidence below; the
[deployment record](../deployment/dev-2026-10-01.md) has the commands, outputs and links. The
[2026-09-08 record](../deployment/production-2026-09-08.md) is kept as history.

## Area status

| Area | Implemented | Exercised locally | Deployed | Exercised in deployment | Capabilities | Contract |
|---|---|---|---|---|---|---|
| Serving and control plane | Yes | Yes | Yes | `server_info`, OpenAPI, MCP `tools/list` (2026-10-01) | `server_info`, `create_customer`, `list_customers` | [HTTP](../serving/http.md), [schema](../control-plane/schema.md) |
| Indexed PostgreSQL session authentication (`0022_session_lookup`) | Yes | See [measurement proof](../build/session-authentication.md) | Yes; the deployed schema is past `0023` | Browser sessions after Clerk sign-in (2026-10-01 review) | `list_sessions`, `revoke_session` | [authentication](../api/auth.md) |
| Identity and SSO | Yes | Yes, including Postgres cases | Yes | No | `sign_in_password`, `begin_passkey_sign_in`, `register_sso_provider`, `scim_list_users` | [identity](../api/identity.md), [SSO](../api/sso.md) |
| Clerk browser sign-in and custodial identities (`0023_custodial_identities`) | Yes | Yes, including Postgres cases | Yes, since `oramasearch/lake:20260925b` (`c045b96`) | A real Clerk `lake` template token exchanged at `/identity/clerk-session` on lake.oramadev.net, signed in (2026-09-25 and 2026-10-01) | — (the `/identity/clerk-session` route) | [Clerk](../api/clerk.md) |
| Design, Train and Ship on per-customer Zetta workspaces, many per space | Yes | Yes, with fake control and workspace callers | Yes | Upload (400 rows), project, pipeline approved and run, run reports, publish, and `predict` from outside the cluster (2026-10-01 review); training on the Getting started CSV fails in Zetta's feature planner ([zetta#1216](https://github.com/oramasearch/zetta/issues/1216)) | `start_workspace`, `create_workspace`, `list_workspaces`, `workspace_status`, `workspace_query`, `workspace_act`, `workspace_sql`, `run_pipeline`, `predict`, `search_index` | [Workspaces](../api/workspaces.md) |
| Solution projects: agent-proposed foundation, policy, holdout, actions and ROI (lake#292) | Yes | Yes: capability and HTTP form tests with a fake workspace; native memory and PostgreSQL suites | Yes | Project `e2e-s1f`: S-1 stages, decisions, a balanced 10% holdout, the ledger and webhook delivery ([#136](https://github.com/oramasearch/orama-bridge/issues/136)) | `start_solution`, `confirm_solution` | [Workspaces](../api/workspaces.md), [guide](../guide/train.md) |
| Managed services: many Zetta and Postgres clusters per customer, `/services` | Yes | Yes: memory and Postgres stores, capabilities, page | Yes | A new Postgres service bound to the `e2e-notes` app ([#136](https://github.com/oramasearch/orama-bridge/issues/136)) | `create_service`, `list_services`, `service_connection`, `connect_service`, `delete_service` | [Services](../api/services.md) |
| Ship Apps desired state, direct-zone reserved-name policy, lifecycle, bindings, logs, events, and list/detail UI (`0034_web_apps`, `0035_web_apps_operator_contract`) | Yes | Yes: built-in/configured name refusal and direct-zone hosts, memory and Postgres stores, purpose-scoped operator hashes, fake operator status/deletion, Loki and bus events, HTTP page/control tests, UI token gates | Yes, with the operator `LakeApp` reconciler | Public `e2e-webhook`, lake-protected `e2e-hello3` and Postgres-bound `e2e-notes` built by the ApplicationAgent and answering on their hosts ([#136](https://github.com/oramasearch/orama-bridge/issues/136)) | `create_app`, `list_apps`, `get_app`, `deploy_app`, `app_logs`, `set_app_auth`, `bind_service_to_app` | [Ship Apps API](../api/web-apps.md), [Ship Apps UI](../ui/apps.md), [operator row](../control-plane/web-apps.md) |
| In-cluster image builds (`0036_builds`) | Yes | Yes: memory and Postgres rows, fake operator status, cancellation, purpose-scoped hashes and retained bus events | Yes, with the operator build reconciler and builder image | `build_app` for `e2e-notes`, pushed to lake's registry and pulled with a per-app token ([#136](https://github.com/oramasearch/orama-bridge/issues/136)) | `build_app`, `build_image`, `get_build`, `list_builds`, `cancel_build`, `build_logs` | [Builds](../api/builds.md), [operator row](../control-plane/builds.md) |
| Train → Streams topic/message CRUD (`0037_streams`) | Yes | Fake workspace/bus callers, 1,000-topic paging/call-count performance case, REST/MCP parity, shared agent scoping, non-consuming reads and creator ownership in the workspace Zetta's records (lake#379; previously PostgreSQL) | Yes | No | `create_stream`, `list_streams`, `get_stream`, `publish_stream_message`, `browse_stream` | [Streams API](../api/streams.md), [Streams UI](../ui/streams.md), [ownership](../control-plane/streams.md) |
| Train → Experiments studies, ExperimentsAgent confirmed start, live metric charts and comparison | Yes | Stateful fake workspace callers, REST/MCP and page parity, per-turn agent scopes, and chart/control checks | Yes | An ExperimentsAgent study of 6 trials, compared and promoted to a registered model ([#136](https://github.com/oramasearch/orama-bridge/issues/136)); a 12-row study fails ([zetta#1217](https://github.com/oramasearch/zetta/issues/1217)) | `list_experiments`, `get_experiment_study`, `start_study`, `stop_study`, `compare_runs`, `promote_trial` | [Experiments API](../api/experiments.md), [Experiments UI](../ui/experiments.md) |
| Service catalog: Kafka, ClickHouse, TiDB, Cassandra, Valkey (preview) | Yes | Yes: kinds, validation, connection for every kind, page | Yes; the upstream operators' CRDs are installed on the cluster | No | `list_service_kinds` | [Services](../api/services.md) |
| Storage phase 1: the type × provider model and a cluster volume per store, Train → Data → Storage (lake#196) | Yes | Yes: memory and Postgres stores (with `NOTIFY lake_services`), five capabilities, page | Yes | No | `create_store`, `list_stores`, `list_store_providers`, `store_connection`, `delete_store` | [Storage](../api/storage.md) |
| Audit and authorization | Yes | Yes | Yes | `my_audit` read in the 2026-10-01 review | `audit`, `my_audit`, `verify_audit`, `effective_access`, `create_grant` | [audit](../api/audit.md), [policy](../api/policy.md) |
| Repositories and sync | Yes | Yes, including Postgres cases | Yes | No | `create_repository`, `list_repositories`, `refs` | [repositories](../api/repositories.md), [sync](../api/sync.md) |
| Forge and export | Yes | Yes, including PostgreSQL review scenario | Yes | No | `create_review`, `create_issue`, `export_repository`, `import_github_repository` | [forge](../api/forge.md), [export](../api/export.md) |
| Search | Yes | Yes | Yes | No | `search` | [search](../api/search.md) |
| Platform and metering (the usage summary across images and workspaces of lake#444 is in the source tree only; see the last row) | Yes | Yes | Yes | `metering_usage` read in the 2026-10-01 review; its numbers are wrong ([lake#444](https://github.com/oramasearch/lake/issues/444)) | `list_environments`, `environment_logs`, `metering_usage` | [platform](../api/platform.md), [metering](../api/metering.md) |
| Secrets, encryption, PKI, and recovery | Yes | Yes, including PostgreSQL identity exit | Yes | No | `put_secret`, `read_secret`, `encrypt`, `issue_x509_svid`, `begin_email_recovery` | [secrets](../api/secrets.md), [PKI](../api/pki.md) |
| Drive and models | Yes | Yes, including PostgreSQL drive scenario | Yes | No | `drive_browse`, `upload_drive_files`, `get_model`, `resolve_model_file` | [drive](../api/drive.md), [models](../api/models.md) |
| Transparency and replication | Yes | Yes | Yes | `transparency_log_configuration` (2026-09-08) | `transparency_log_configuration`, `transparency_checkpoint`, `replicate_transparency_log` | [transparency](../api/transparency.md), [replication](../api/replication.md) |
| Application and agent services | Yes | Yes | Yes | No | `register_application`, `list_applications`, `issue_agent_certificate` | [applications](../api/apps.md), [agents](../api/agents.md) |
| Server-rendered UI | Yes | Yes | Yes | Every customer job in the browser at 1440 and 390 px (2026-10-01 review) | — (pages, not capabilities) | [UI](../ui/README.md) |
| Ask Orama, Pipelines as agent tools, Discover Opportunities, the Agent Factory | Yes | Yes | Yes | Ask Orama answered a two-part question and handed off to the ExperimentsAgent (2026-10-01 review); Discover and the Agent Factory were not exercised | `agent_turn`, `get_ask_conversation`, `list_opportunities`, `create_projects_from_opportunities`, `create_agent_from_proposal` | [ask-orama](../api/ask-orama.md), [train-agents](../api/train-agents.md), [discover](../api/discover.md), [agent-factory](../api/agent-factory.md) |
| Agent launches, schedules and turns (`agent_turn`) | Yes | Yes | Yes | `agent_turn` answered; *Run once* stays at phase `""` ([lake#442](https://github.com/oramasearch/lake/issues/442)) | `launch_agent`, `list_agent_launches`, `stop_agent_launch`, `schedule_agent`, `agent_turn` | [agent-launches](../api/agent-launches.md) |
| Renewable agent-task claims and three-attempt recovery (`0042_agent_task_leases`) | Yes | Memory/PostgreSQL regressions; native proof recorded with the PR | Yes; the operator vendors the schema through `0042` | Library agents claimed and answered tasks during the [#136](https://github.com/oramasearch/orama-bridge/issues/136) runs; lease expiry and retry were not provoked | `claim_agent_task`, `receive_agent_tasks`, `get_agent_task` | [agent bus](../api/agent-bus.md) |
| Never-expiring agent keys and tokens (opt-in) | Yes | Yes | Yes | No | `create_token`, `mint_agent_session_key` | [agents](../api/agents.md#keys-that-never-expire-lake234), [identity](../api/identity.md) |
| Train → Agents and the Agent Library | Yes | Yes | Yes | The Agent Library and *Talk to* list in the browser; created agents are not listed ([lake#442](https://github.com/oramasearch/lake/issues/442)) | `list_agent_library` | [train-agents](../api/train-agents.md) |
| Agents reach Zetta through lake (`/workspaces/<customer>/zetta/v1/`) | Yes | Yes | Yes | No | `workspace_openapi` | [workspaces](../api/workspaces.md#agents-reach-zetta-through-lake) |
| Organizations in the browser: create an organization space, join one, convert a personal space ([lake#461](https://github.com/oramasearch/lake/pull/461), lake#443) | Yes | Yes: memory and Postgres stores, pages, signed-up-person output checks | No; merged after the 2026-10-01 observation | No | `create_organization_space`, `join_organization`, `convert_personal_space` | [organizations](../api/organizations.md), [settings UI](../ui/settings.md) |
| Merged after the 2026-10-01 observation, without new capabilities: Project Agent applies and checks its proposals ([#454](https://github.com/oramasearch/lake/pull/454)), `workspace_id` in every copy-paste example ([#455](https://github.com/oramasearch/lake/pull/455)), 500 for a throwing handler and Packages with queued builds ([#456](https://github.com/oramasearch/lake/pull/456)), MCP tool-list change notices and one schema for REST and MCP ([#458](https://github.com/oramasearch/lake/pull/458)), dataset names and upload checks on the Data page ([#459](https://github.com/oramasearch/lake/pull/459)), usage counting workspace data, images, builds and runs ([#462](https://github.com/oramasearch/lake/pull/462)) | Yes | Yes, in each pull request's tests | No | No | — (changes to existing capabilities and pages) | [workspaces](../api/workspaces.md), [MCP](../api/mcp.md), [metering](../api/metering.md) |
| API consistency (lake#450): one documented error-code table, 401 `unauthenticated` with `missing`/`invalid`/`expired` and a key link, `path` on engine refusals, `customer_id` blamed for a space that does not own the workspace, empty query keys ignored, one name per concept with deprecated aliases, `expose_model`/`expose_index` answering the lake call, `predict` entity keys, refused unknown token scopes, rewritten OpenAPI descriptions | Yes | Memory/PostgreSQL regressions (`ApiErrors`, `ApiNaming`, `ApiDescriptions`) | No; merged after the 2026-10-01 observation | No | — | [errors](../api/errors.md), [naming](../api/naming.md), [auth scopes](../api/auth.md#scopes) |
| Workspace records, and Ask Orama conversations in the owning workspace's Zetta (`0045_ask_message_workspaces`, lake#370): sealed `lake_*` collections in the workspace Zetta's record store (needs Zetta dev with zetta#1233), `get_ask_conversation` by `workspace_id`, the operator's one-shot copy and verification | Yes | Memory/PostgreSQL regressions, plus an opt-in run against a local Zetta node built from Zetta dev `82f617e` | No; operator schema vendoring and workspace Zettas on dev with #1233 required, and the copy has not run on dev | No | `copy_workspace_records` | [workspace records](../architecture/workspace-records.md), [Ask Orama](../api/ask-orama.md) |
| Design → Opportunities in the owning workspace's Zetta (`0046_opportunity_workspaces`, lake#376): the scan's workspace keeps its Opportunities, `list_opportunities`/`dismiss_opportunity`/`create_projects_from_opportunities` by `workspace_id`, conditional status writes, the one-shot copy of the retired table | Yes | Memory/PostgreSQL regressions | No; operator schema vendoring and workspace Zettas on dev with #1233 required, and the copy has not run on dev | No | `copy_workspace_records` | [workspace records](../architecture/workspace-records.md), [Design → Opportunities](../api/discover.md) |
| Drive share links in the workspace Zetta that holds them (`0048_drive_share_link_workspaces`, lake#350): `workspace_id` on create, list and revoke, self-routing bearer tokens, content-free routes for pre-move tokens, the one-shot copy of `drive_share_links` | Yes | Memory/PostgreSQL regressions and the signed-out recipient scenario | No; operator schema vendoring and workspace Zettas with zetta#1233 required, and the copy has not run on dev | No | — (changes to existing capabilities and pages) | [sharing](../api/sharing.md#where-links-live), [workspace records](../architecture/workspace-records.md) |
| Application lifecycle events and webhook delivery attempts in the application's workspace Zetta (`0049_application_webhook_workspaces`, lake#356, lake#357): `register_application` takes `workspace_id`, each attempt is claimed before it is sent, the copy covers both tables | Yes | Memory/PostgreSQL regressions | No; operator schema vendoring and workspace Zettas with zetta#1233 required, and the copy has not run on dev | No | `copy_workspace_records` (existing application capabilities changed) | [apps](../api/apps.md#where-events-and-attempts-live-lake356-lake357), [workspace records](../architecture/workspace-records.md) |
| Repository workspaces (`0051_repository_workspaces`, lake#315 part 1): `workspace_id` required on `create_repository`, `import_repository`, `import_github_repository`, the `/repositories` form and hub `create_repo`; immutable routing (transfer refused); returned on reads and carried through push authorization; workspace deletion refused while it holds a live repository; repository pages show the workspace | Yes | Memory/PostgreSQL regressions, migration test | No; operator schema vendoring required | No | — (changes to existing capabilities and pages) | [repositories](../api/repositories.md#the-owning-workspace) |
| Workspace-scoped stores and the operator SQL contract test (`0044_store_workspaces`, lake#465) | Yes | Memory/PostgreSQL regressions | No; operator schema vendoring required | No | — | [storage](../control-plane/storage.md) |
| Workspace-scoped agent launches and schedules, launch start timeout, definition field checks (`0043_agent_launch_workspaces`, lake#442) | Yes | Memory/PostgreSQL regressions | No; merged after the 2026-10-01 observation (operator schema vendored in orama-operator#244) | No | — | [agent-launches](../api/agent-launches.md) |

## Keeping this page true

When behavior changes, update its contract page and this table in the same change. When a new
deployment is observed, add a dated deployment record with commands and outputs, then change only
the deployed and exercised-in-deployment cells supported by that evidence. Do not infer
end-to-end behavior from source tests or route discovery.

The *Deployed* cell starts with **Yes** or **No**, and the *Capabilities* cell names the registry
operations the row speaks for. `tools/docs/check-status.py` fails when a row says **No** while the
observed OpenAPI lists one of its capabilities, says **Yes** while one is missing, names a
capability the source registry does not have, or does not link the newest snapshot under
`docs/deployment/observed/`. It also fails when the source registry has a capability the observed
OpenAPI lacks and no **No** row names it: a capability merged after the last observation is
recorded as not deployed, never left out. `DocsStatus.MatchesTheObservedOpenApi` runs it in ctest (and CI)
against that snapshot. After a deployment, record a new snapshot and run the same check against
the live service:

```sh
tools/docs/check-status.py --observed https://lake.oramadev.net/v1/openapi.json \
  --snapshot docs/deployment/observed/openapi-$(date +%F).json --revision <server_info revision>
tools/docs/check-status.py --observed https://lake.oramadev.net/v1/openapi.json
```

then update this page's Status line and link to name the new snapshot.
