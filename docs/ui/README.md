# The web UI

lake's console is a React single-page app in `frontend/` (lake#561). It replaced the
server-rendered pages (`src/api/web_ui*.cpp`). Lake embeds the built app in its binary and
answers every browser page request with `index.html`. The app then calls only lake's `/v1`
capabilities, the same ones REST and MCP callers use, with the same authorization and audit. It
can do nothing the API cannot.

- How the app is built, its kit and the rules for a page: [web-app.md](web-app.md).
- The Train and Data pages (Agents, Agent, Agent Factory, Runs, Run, Datasets, Dataset, Query):
  [train.md](train.md).
- The Ship pages (Models, Search Indexes, Search Index, Services): [ship.md](ship.md).
- How it works on a phone, and how to check it at 390×844: [mobile-web.md](mobile-web.md).
- Data → Transcripts, with dictation, recording and upload: [transcripts.md](transcripts.md).
- The Organization, Administration and Account pages: [organization.md](organization.md).
- The capabilities added for the app (`viewer`, `my_permissions`, `list_packages`, `list_docs`,
  `get_doc`, `mcp_setup`, `resolve_model`, `latest_agent_task`, `compile_pipeline`,
  `render_pipeline`): [../api/web-app.md](../api/web-app.md).

## What lake serves

| Request | Answer |
|---|---|
| A browser `GET`/`HEAD` that accepts `text/html`, for a path no server route owns | `index.html`, `Cache-Control: no-cache`; the app routes it (`web_app_serves`, `src/api/web_app.cpp`) |
| `/ui/<file>` | the content-hashed scripts, styles and fonts Vite built, `public, max-age=31536000, immutable` |
| `/v1/<capability>`, `/w/<workspace_id>/v1/<capability>`, `/mcp` | the API ([../api/README.md](../api/README.md)) |
| `POST /session`, `POST /logout`, `POST /identity/clerk-session`, the OAuth callback | sign-in and sign-out, which set or clear the HttpOnly `lake_session` cookie |
| `/file/raw`, `/file/download`, `/drive/download`, `/exports/download`, `/share/<token>/download` | bytes, as attachments or plain text |
| `/apps/<id>/*`, custom sign-in domains, `/apps/authorize`, `/v1/apps/auth` | an identity application's own hosted pages, still rendered by lake ([identity.md](identity.md#application-identity)) |
| `/llms.txt`, `/llms-full.txt`, `/skills/…`, `/developer/mcp/skill`, `/v1/openapi.json` | documents for agents and API clients |

The tests are `WebApp.*` in `tests/test_web_app.cpp`. A request that does not accept HTML (npm,
pip, git, API clients) never gets the app.

## The shell

`frontend/src/components/Shell.tsx` frames every page except the bare ones (sign-in, sign-up,
operator sign-in, passkey sign-in and the public share pages).

- **The rail** lists lake's groups in order: the top items, Design, Train, Ship, then Organization
  for an account or Administration for the operator, then Developer. The items come from `NAV` in
  `frontend/src/routes.tsx`, and the viewer matrix is in [front-door.md](front-door.md#navigation).
  The current group is open; the others show their name and open their first page. The rail
  collapses to icons (remembered in `localStorage["lake-rail"]`). Below 1024 px it is an
  off-canvas drawer opened from the menu button.
- **The top bar** holds the space and workspace picker ([workspace-selection.md](workspace-selection.md)),
  the breadcrumb ([breadcrumb.md](breadcrumb.md)), **Go to…** (⌘K or `/`), the theme switch, Ask
  Orama and the account menu. **G** then **D**, **T**, **S**, **O** or **A** jumps to that group's
  first page.
- **Ask Orama** opens beside the page (`components/AskPanel.tsx`) in the current workspace. The
  conversation is kept per workspace for the browser tab (`sessionStorage`). **Full view** opens
  the same conversation on `/ask`. Before, this was a docked iframe of the server's Ask page. Its
  message box has the microphone (dictation, `components/dictation/Dictate.tsx`) and **From your
  phone**; a page can open it about something (`openAsk`, `lib/ask.ts`), as the transcript page
  does.
- **On a phone** the top bar is compact, Ask Orama is a full-screen conversation, a `Modal sheet`
  fills the screen with its actions above the keyboard, and every page fits 390 px:
  [mobile-web.md](mobile-web.md).
- **A 401 means the session ended.** Any read or write answering 401 while someone is signed in
  shows *Your session has ended* with **Sign in again**, which returns to the same address. The
  page stays as it was (`components/SessionEnded.tsx`).
- **A render error stays in its page.** `components/ScreenBoundary.tsx` shows the error and keeps
  the shell working.
- **Who may open a page** is the screen's `audience` in its `routes.ts`. A visitor opening an
  account page is sent to `/sign-in?next=<the page>`. The capability still decides what the page
  can read.

## Pages

Every page is registered once, in its group's `frontend/src/pages/<group>/routes.ts`. The router,
the rail, the breadcrumb and Go to read that registry. A page marked `workspace: true` is served at
`/w/<workspace_id><path>`, and its unprefixed address opens the current workspace
([workspace-selection.md](workspace-selection.md)). Design, Train and Ship reach the workspace's
engine through `workspace_query` (a read sent as `POST`) and `workspace_act`. In the table, *engine*
names the engine capability passed to them.

Files are under `frontend/src/pages/`.

**Front door, account and developer**

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/` | `home/HomePage.tsx` | what lake is for and the viewer's next step ([front-door.md](front-door.md)) | `viewer` |
| `/sign-in`, `/sign-up`, `/mfa`, `/passkey-sign-in` | `account/SignInPage.tsx`, `SignUpPage.tsx`, `MfaPage.tsx`, `PasskeySignInPage.tsx` | lake's own sign-in and sign-up, or Clerk's (`account/ClerkSignIn.tsx`); see [identity.md](identity.md) | `sign_in_password`, `verify_mfa`, `start_oauth`, `sign_up_password`, `begin_passkey_sign_in`, `sign_in_passkey`, `enroll_mfa`, `disable_mfa` |
| `/login` | `account/LoginPage.tsx` | the operator-token form, posted to `POST /session` | none |
| `/settings`, `/sessions`, `/passkeys`, `/tokens` | `account/SettingsPage.tsx`, `SessionsPage.tsx`, `PasskeysPage.tsx`, `TokensPage.tsx` | profile, sessions and devices, passkeys, tokens ([settings.md](settings.md#personal-settings)) | `list_sessions`, `revoke_session`, `list_devices`, `revoke_device`, `begin_passkey_registration`, `register_passkey`, `create_token` |
| `/docs`, `/docs/:page` | `developer/DocsPage.tsx` | lake's guides (`docs/guide/*.md`) | `list_docs`, `get_doc` |
| `/developer/api` | `developer/ApiPage.tsx` | a base-URL strip, then tabs Keys (make, list and revoke API keys), Quick start (curl and fetch against the viewer's origin) and Operations (every `/v1/openapi.json` operation, grouped, searchable, filtered by read or write, with its schemas and a curl line); downloads `openapi.json` ([../api/mcp.md](../api/mcp.md#developer--api-and-developer--mcp-lake561)) | `mcp_setup`, `create_token`, `list_sessions`, `revoke_session`, `GET /v1/openapi.json` |
| `/developer/mcp` | `developer/McpPage.tsx` | one page after rev-os: an endpoint strip, then numbered steps: create a key (shown once) or paste one; set up your client (tiles for Claude Code, Codex, Claude Desktop, Cursor, VS Code, Windsurf, Gemini CLI, any client and the web connectors, each with its file, copyable blocks and a check); teach your agent (the skill and an install per agent); what the tools do (group and read/write chips with counts, search); worked flows; your keys ([../api/mcp.md](../api/mcp.md#developer--api-and-developer--mcp-lake561)) | `mcp_setup`, `create_token`, `list_sessions`, `revoke_session` |

**Repositories, Drive and sharing** ([repositories.md](repositories.md), [forge.md](forge.md),
[drive.md](drive.md), [sharing.md](sharing.md))

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/repositories` | `repositories/RepositoriesPage.tsx` | readable repositories (public ones for a visitor) and Create a Repository | `list_repositories`, `create_repository` |
| `/repository?id=` | `repositories/RepositoryPage.tsx` | the repository home | `get_repository`, `repository_tree`, `repository_refs`, `repository_file`, `list_changes`, `get_customer`, `get_workspace`, `list_releases`, `entity_effective_access`, `build_image` |
| `/repository/tree?id=&ref=` | `repositories/TreePage.tsx` | every path with its newest change | `get_repository`, `repository_tree`, `list_changes` |
| `/file?repository_id=&ref=&path=` | `repositories/FilePage.tsx` | one file, line anchors, blame | `get_repository`, `repository_file`, `list_changes`, `change_diff` |
| `/changes`, `/change` | `repositories/ChangesPage.tsx`, `ChangePage.tsx` | the change log; one change under review | `list_changes`, `get_change`, `change_diff`, `list_reviews`, `list_review_comments`, `list_artifacts` |
| `/issues`, `/issue` | `repositories/IssuesPage.tsx`, `IssuePage.tsx` | issues; one issue and its discussion | `list_issues`, `list_issue_comments` |
| `/releases` | `repositories/ReleasesPage.tsx` | releases with notes and attested artifacts | `list_releases` |
| `/webhooks` | `repositories/WebhooksPage.tsx` | webhooks and their deliveries, with retry | `list_webhooks`, `list_webhook_deliveries`, `retry_webhook_delivery` |
| `/protection` | `repositories/ProtectionPage.tsx` | branch protection rules and refused updates | `list_branch_protections`, `audit` |
| `/conflicts` | `repositories/ConflictsPage.tsx` | unresolved conflicts and their resolution | `list_conflicts`, `resolve_conflict` |
| `/artifacts`, `/artifact` | `repositories/ArtifactsPage.tsx`, `ArtifactPage.tsx` | build results; one result | `list_artifacts`, `get_artifact` |
| `/environments`, `/environment`, `/logs`, `/exports` | `apps/EnvironmentsPage.tsx`, `EnvironmentPage.tsx`, `LogsPage.tsx`, `ExportsPage.tsx` | a repository's environments, one environment, its logs, its exports | `list_environments`, `get_environment`, `list_deployments`, `create_domain`, `delete_domain`, `environment_logs`, `list_exports` |
| `/drive`, `/drive/preview`, `/drive/history`, `/drive/history/diff`, `/drive/conflict` | `repositories/DrivePage.tsx`, `DrivePreviewPage.tsx`, `DriveHistoryPage.tsx`, `DriveDiffPage.tsx`, `DriveConflictPage.tsx` | folders, preview, history, version comparison, conflicts | `drive_browse`, `create_drive_folder`, `upload_drive_files`, `move_drive_path`, `delete_drive_path`, `drive_preview`, `drive_history`, `restore_drive_version`, `diff_drive_versions`, `drive_conflict`, `resolve_conflict` |
| `/drive/share`, `/drive/share/link` | `repositories/DriveSharePage.tsx`, `DriveShareLinkPage.tsx` | who can reach a folder, sharing, links | `drive_effective_access`, `my_permissions`, `share_drive_folder`, `create_drive_share_link`, `revoke_drive_share_link`, `list_workspaces` |
| `/shared-with-me` | `repositories/SharedWithMePage.tsx` | folders shared with the viewer | `drive_shared_with_me` |
| `/share/:token`, `/share/:token/preview` | `repositories/SharePage.tsx`, `SharePreviewPage.tsx` | a folder or file opened through a link, signed out | `resolve_drive_share_link`, `preview_drive_share_link` |
| `/models/:namespace/:id`, `/models/:id` | `admin/ModelPage.tsx` | a model repository's storage, provenance, files and revisions | `resolve_model`, `get_model` |
| `/search` | `search/SearchPage.tsx` | one query across the places the person ticks (`?q=&in=`): Repositories, Code (ranked, substring or regex, with its options), Datasets, Transcripts, Projects, Agents, Models and Docs, searched side by side and grouped by place; the choice is remembered in this browser, and `?query=` still opens code search | `list_repositories`, `search`, `workspace_sql`, `list_transcripts`, `workspace_query` (`list_projects`, `list_agents`, `list_models`), `list_docs`, `get_doc` |

**Design, Train and Ship** (all under `/w/<workspace_id>/` except Deployments, Services and
Workspaces)

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/design/projects` | `design/ProjectsPage.tsx` | projects, New Project | engine `list_projects`, `create_project` |
| `/design/project?name=` | `design/ProjectPage.tsx`, `SolutionPanels.tsx` | one project, its solution panels, runs, background tasks, pipelines | `latest_agent_task`, `list_runs`, `compile_pipeline`, `create_pipeline`, `run_pipeline`, `confirm_solution`, `workspace_sql`; engine `get_project`, `get_graph`, `list_datasets`, `list_methodologies`, `approve_plan`, `delete_project`, solution reads and actions |
| `/design/pipeline?project=&graph=&step=` | `design/PipelinePage.tsx` | the pipeline graph, a step, changes, gate and deploy | `render_pipeline`, `edit_pipeline`, `run_pipeline`, `agent_turn`, `get_agent_task`; engine `get_graph`, `list_datasets`, `list_runs`, `gate`, `deploy`, `approve_plan` |
| `/design/opportunities`, `/design/discover` | `design/OpportunitiesPage.tsx` | Auto-Suggest's opportunities, drafts on confirmation | `list_opportunities`, `create_projects_from_opportunities`, `dismiss_opportunity`, `agent_turn`, `get_agent_task` |
| `/design/solutions`, `/design/solution` | `design/SolutionsPage.tsx`, `SolutionPage.tsx` | the solutions catalogue; Start This Solution | `start_solution`; engine `list_solutions`, `get_solution`, `list_datasets` |
| `/design/methodologies`, `/design/methodology` | `design/MethodologiesPage.tsx`, `MethodologyPage.tsx` | modelling methods | engine `list_methodologies`, `list_solutions` |
| `/design/agent-library` | `design/AgentLibraryPage.tsx` | the agents Orama provides | `list_agent_library` |
| `/train/agents`, `/train/agent`, `/train/agents/new` | `train/AgentsPage.tsx`, `AgentPage.tsx`, `FactoryPage.tsx` | the space's agents, one agent, the Agent Factory | `schedule_agent`, `unschedule_agent`, `launch_agent`, `stop_agent_launch`, `list_agent_launches`, `list_agent_schedules`, `approve_agent_proposal`; engine `list_agents`, `create_agent`, `get_agent`, `update_agent`, `delete_agent`, `agent_training_status` |
| `/train/data`, `/train/dataset`, `/train/query` | `train/DataPage.tsx`, `DatasetPage.tsx`, `QueryPage.tsx` | load data, connections, datasets, storage; one dataset; read-only SQL | `list_stores`, `create_store`, `delete_store`, `store_connection`, `list_services`, `connect_service`, `workspace_sql`; engine connection, dataset and generator calls |
| `/transcripts`, `/transcript?id=` | `transcripts/TranscriptsPage.tsx`, `TranscriptPage.tsx`, `NewTranscript.tsx` | Data → Transcripts: record, upload or paste a transcript; search, read, edit, copy, delete, Ask Orama about it ([transcripts.md](transcripts.md)) | `list_transcripts`, `get_transcript`, `create_transcript`, `update_transcript`, `delete_transcript`, `transcribe` |
| `/train/runs`, `/train/run` | `train/RunsPage.tsx`, `RunPage.tsx` | pipeline runs; one run's report, lineage, log and trace | engine `list_runs`, `list_studies`, `get_run`, `get_run_report`, `list_traces` |
| `/train/experiments`, `/train/experiment`, `/train/experiment/run`, `/train/experiments/compare` | `experiments/*.tsx` | studies and runs with live charts ([experiments.md](experiments.md)) | see experiments.md |
| `/train/streams`, `/train/stream` | `streams/StreamsPage.tsx`, `TopicPage.tsx` | topics and messages ([streams.md](streams.md)) | see streams.md |
| `/ship/models` | `ship/ModelsPage.tsx` | trained models and their prediction endpoints | engine `list_models`, `expose_model`, `unexpose_model` |
| `/ship/indexes`, `/ship/index` | `ship/IndexesPage.tsx`, `IndexPage.tsx` | search indexes, one index, search it | `search_index`, `create_token`; engine `list_indexes`, `create_index`, `get_index`, `expose_index`, `unexpose_index` |
| `/ship/packages` | `apps/PackagesPage.tsx` | the instance registry, the space's images and builds | `list_packages`, `list_image_repositories`, `list_builds`, `receive_build_events` |
| `/ship/apps`, `/ship/app` | `apps/AppsPage.tsx`, `AppPage.tsx` | apps, ApplicationAgent, one app ([apps.md](apps.md)) | see apps.md |
| `/deployments` | `apps/DeploymentsPage.tsx` | every environment across readable repositories | `list_repositories`, `list_environments` |
| `/services` | `ship/ServicesPage.tsx` | the space's databases, streams and engines | `list_services`, `list_service_kinds`, `create_service`, `delete_service`, `service_connection` |
| `/workspaces` | `workspaces/WorkspacesPage.tsx` | a space's workspaces, create, delete ([workspace-selection.md](workspace-selection.md#the-workspaces-page)) | `list_workspaces`, `create_workspace`, `delete_workspace` |
| `/ask` | `ask/AskPage.tsx` | Ask Orama, full page ([../api/ask-orama.md](../api/ask-orama.md)) | `send_ask_message`, `settle_ask_turn`, `confirm_ask_proposal`, `get_ask_conversation`, `get_agent_task`, `list_agent_library` |

**Organization and administration** ([settings.md](settings.md), [identity.md](identity.md))

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/organizations` | `organizations/OrganizationsPage.tsx` | organizations, invitations for you, create, convert | `list_organizations`, `list_invitations`, `join_organization`, `create_organization_space`, `convert_personal_space`, `get_customer` |
| `/organization?id=&tab=` | `organizations/OrganizationPage.tsx` | one organization's tabs | `get_organization`, `list_teams`, `create_team`, `list_memberships`, `list_invitations`, `create_invitation`, `entity_effective_access`, `list_sso_providers`, `register_sso_provider`, `list_applications`, `metering_usage`, `audit` |
| `/team`, `/teams`, `/member`, `/members`, `/invitations`, `/grants` | `organizations/TeamPage.tsx`, `TeamsPage.tsx`, `MemberPage.tsx`, `MembersPage.tsx`, `InvitationsPage.tsx`, `GrantsPage.tsx` | teams, memberships, invitations, direct grants | `get_team`, `list_teams`, `get_membership`, `set_member_role`, `remove_member`, `list_memberships`, `list_invitations`, `list_grants`, `get_object` |
| `/access`, `/repository/access`, `/organization/access`, `/team/access` | `organizations/AccessPage.tsx` | effective access to one object | `effective_access`, `entity_effective_access`, `get_object` |
| `/sso` | `organizations/SsoPage.tsx` | SSO providers, federation issuers, a SCIM log | `list_sso_providers`, `list_federation_issuers`, `list_sso_provisioning_logs` |
| `/customers` | `admin/CustomersPage.tsx` | every space | `list_customers` |
| `/applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | `admin/Application*.tsx` | identity applications ([identity.md](identity.md#application-identity)) | `list_applications`, `register_application`, `get_application`, `list_application_users`, `suspend_application_user`, `list_application_organizations`, `create_application_organization`, `delete_application_organization`, `list_application_webhook_events`, `create_application_webhook` |
| `/metering?customer_id=` | `admin/MeteringPage.tsx` | a space's usage and storage limit | `metering_usage`, `set_storage_quota` |
| `/secret-mounts`, `/secrets`, `/encryption-keys` | `admin/SecretMountsPage.tsx`, `SecretsPage.tsx`, `EncryptionKeysPage.tsx` | secret mounts, a secret's version metadata, named keys; never values or key material | `list_secret_mounts`, `get_secret_mount`, `secret_metadata`, `list_encryption_keys` |
| `/activity`, `/audit` | `admin/AuditPage.tsx` | the person's own events; the operator's whole log | `my_audit`, `audit`, `transparency_log_configuration` |
| `/transparency?identity=` | `admin/TransparencyPage.tsx` | the public checkpoint-verified scan for one TAAP identity | `transparency_identity_scan` |
| any other path | `home/NotFoundPage.tsx` | *not found* in lake's words | none |

## Rules the pages keep

- **Public reads open signed out.** Public repositories, public models, the Docs, the transparency
  monitor and share links open without a session. A refused repository read for a visitor goes to
  `/sign-in?next=…`, so a private repository's existence is not disclosed
  (`RepositoryError` in `pages/repositories/forge.tsx`).
- **Secrets never reach the page.** The secrets and encryption-key pages show metadata only. A new
  token, an MFA secret and recovery codes are shown once and held only in that page's memory.
- **Lifecycle writes stay capability-only.** The repository page shows a deletion tombstone and its
  retention deadline; it has no lifecycle form.
- **Workspace data is read where it lives.** Forge records, environments and deployments, app
  records and repository descriptions are in the owning workspace's Zetta. A page that can show
  the rest says when that workspace cannot be read, instead of failing
  ([repositories.md](repositories.md), [../api/platform.md](../api/platform.md#where-environments-live-lake358-lake359)).
  Activity and Audit read the central log and each workspace's audit, name the log each event came
  from, and name a workspace whose audit could not be read ([../api/audit.md](../api/audit.md#reading-both-logs)).

## What changed from the server pages

- No page refreshes itself. Pages that followed work with `<meta http-equiv="refresh">` now poll
  or long-poll the capability the server page's script called, and show the change in place.
- The deferred panels (`/ask/agents`, `/train/agents/list`, `/ship/indexes/datasets`) are gone. The
  page reads the capability itself and fills the panel in when it answers.
- The form `POST` adapters (`/identity/*` form posts, `/drive/*`, `/ship/app/*`, `/design/*`,
  `/train/*`, `/organization/*`, …) are gone. Every change is a `POST /v1/<capability>` with a
  JSON body. The confirmations those handlers enforced (typed names, counts, the reviewed
  proposal) are enforced by the page before the call, and by the capability where it has its own
  check.
- The `lake_workspace` cookie and the server's rewriting of links and forms into `/w/<id>/` are
  gone. The workspace is in the page URL ([workspace-selection.md](workspace-selection.md)).
- Markdown, file highlighting and the charts are drawn in the browser. The pipeline graph
  (`render_pipeline`), Drive previews and syntax units (`repository_file {units: true}`) are still
  produced by lake.
- Lake had no HTML 404; the app has one. Defects the inventory found in the server pages are not
  carried over: device revoke sends `id`, the environment log page appends lines, the app log
  panel reads `items`, a plain member opens an organization, and Docs is marked current in the rail.

## Screenshots

`docs/ui/screenshots/` holds images of the **server-rendered UI**. They are kept for comparison
with the web app and are not regenerated.

| Folder | What |
|---|---|
| `before/` | 120 PNGs: the seeded fixture's pages before the Orama design system was applied (#87), light and dark |
| `after/` | 124 PNGs: the same pages after it, light and dark, at 1440 × 900 ([design-system.md](design-system.md#seeded-screenshot-fixture)) |
| `front-door/` | 16 PNGs and a README: the 2026-09-08 homepage journey at 1280 and 640 px ([front-door.md](front-door.md#browser-evidence)) |
| `sign-up/` | 16 PNGs and a README: the 2026-09-08 sign-up walkthrough ([sign-up-walkthrough.md](sign-up-walkthrough.md)) |
| `issue-122/` | 8 PNGs: a share link's folder and nested preview ([sharing.md](sharing.md#recipient-walkthrough-evidence)) |

## Verify

```sh
cd frontend && npm ci && npm run build      # types, unit tests, the bundle and the source checks
cmake --build build && ctest --test-dir build -R '^WebApp\.'
./build/lake --listen 127.0.0.1:4902 --memory-store --admin-token t
curl -s -H 'Accept: text/html' localhost:4902/repositories | head -3   # index.html
curl -s localhost:4902/v1/viewer                                        # a visitor
```

Then open `http://127.0.0.1:4902/login`, sign in with `t`, and walk the rail.

## Change

Add a page by adding a screen to its group's `routes.ts` and its component under `pages/<group>/`;
it then appears in the router, the rail (when `nav` is set), the breadcrumb and Go to. Anything the
page needs that no capability returns is a new capability first ([../api/web-app.md](../api/web-app.md#change)).
Update the page table above in the same change.
