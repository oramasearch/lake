# The web UI

lake's console is a React single-page app in `frontend/` (lake#561). It replaced the
server-rendered pages (`src/api/web_ui*.cpp`). Lake embeds the built app in its binary and
answers every browser page request with `index.html`. The app then calls only lake's `/v1`
capabilities, the same ones REST and MCP callers use, with the same authorization and audit. It
can do nothing the API cannot.

- How the app is built, its kit and the rules for a page: [web-app.md](web-app.md).
- The Train and Data pages (Agents, Agent, Agent Factory, Runs, Run, Models, Model, Datasets,
  Dataset, Query): [train.md](train.md). Agents are designed in Design → Agents and trained in
  Train → Agents (lake#653, [../api/train-agents.md](../api/train-agents.md)).
- The Ship pages (Models, Model, Search Indexes, Search Index, Services): [ship.md](ship.md).
  Train → Models is about training a model, Ship → Models about serving it (lake#599).
- How it works on a phone, and how to check it at 390×844: [mobile-web.md](mobile-web.md).
- Data → Transcripts, with dictation, recording and upload: [transcripts.md](transcripts.md).
- Data → Drives, one file manager over repository, workspace-files, virtual and storage drives,
  and its Storage tab (lake#656): [drives.md](drives.md).
- Data → Connections, the databases a workspace reads (lake#651): [connections.md](connections.md).
- The Organization, Administration and Account pages: [organization.md](organization.md).
- Deleting and revoking (organizations, invitations, grants, secret mounts, datasets, models,
  runs, search indexes, Ask Orama conversations): one typed confirmation that first lists what
  goes, [deletion.md](deletion.md).
- The catalogue icons of every solution and methodology, their style sheet and motif dictionary:
  [catalogue-icons.md](catalogue-icons.md).
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
| `/file/raw`, `/file/download`, `/drive/download`, `/drive/files/download`, `/exports/download`, `/share/<token>/download` | bytes, as attachments or plain text (a workspace file streamed, [../api/drives.md](../api/drives.md#uploads)) |
| `/apps/<id>/*`, custom sign-in domains, `/apps/authorize`, `/v1/apps/auth` | an identity application's own hosted pages, still rendered by lake ([identity.md](identity.md#application-identity)) |
| `/llms.txt`, `/llms-full.txt`, `/skills/…`, `/developer/mcp/skill`, `/v1/openapi.json` | documents for agents and API clients |

The tests are `WebApp.*` in `tests/test_web_app.cpp`. A request that does not accept HTML (npm,
pip, git, API clients) never gets the app.

## The shell

`frontend/src/components/Shell.tsx` frames every page except the bare ones (sign-in, sign-up,
operator sign-in, passkey sign-in and the public share pages).

- **The rail** lists lake's groups in order: the top items (for a signed-in person the **Home**
  group: **Ask Orama** at `/`, then **Dashboard** at `/dashboard`; lake#600), Data, Design, Train,
  Ship, then Organization
  for an account or Administration for the operator, then Developer. The items come from `NAV` in
  `frontend/src/routes.tsx`, and the viewer matrix is in [front-door.md](front-door.md#navigation).
  The current group is open; the others show their name and open their first page. The rail
  collapses to icons (remembered in `localStorage["lake-rail"]`). Below 1024 px it is an
  off-canvas drawer opened from the menu button.
- **The top bar** holds the workspace picker, grouped by owner ([workspace-selection.md](workspace-selection.md#the-picker)),
  the breadcrumb ([breadcrumb.md](breadcrumb.md)), **Search** (⌘K or `/`), the theme switch, Ask
  Orama and the account menu. **G** then **D**, **T**, **S**, **O** or **A** jumps to that group's
  first page.
- **The top bar's search searches everything** (`components/CommandPalette.tsx`, lake#633). As the
  person types it shows **Go to** (pages and workspaces, filtered at once) and then every place
  Data → Search covers: repositories, code, datasets, transcripts, projects, solutions, agents, models, drives
  and docs, each place's best four hits with the match marked and a muted second line saying where
  it lives (a transcript's is the text around the match). Workspace places are searched in every
  workspace the person has, and a hit names its workspace when there are several. Places are asked
  from two characters, 180 ms after typing stops; each place fills in as it answers, a slow place
  shows *Searching…* without holding the others, a failed one shows one muted line in its group, a
  place that found nothing is left out, and a stale query is cancelled. ↑ ↓ move across groups, ↵ opens the item as Data → Search would, and ⌘↵
  or **See all results** opens `/search?q=…&in=<every place>`. Until the person moves, ↵ opens the
  first page of Go to, or, with none, See all results, so a choice never jumps while places answer.
  On a phone it fills the screen. The places, their reads and the hits are `lib/federatedPlaces.tsx`,
  shared with Data → Search (one query cache, so See all opens with the answers already there); the
  rows, ranking, workspace merging and excerpts are `lib/federated.ts`, tested by
  `tests/federated.test.ts`.
- **Ask Orama** opens beside the page (`components/AskPanel.tsx`) in the current workspace. The
  conversation is kept per workspace for the browser tab (`sessionStorage`). The panel runs from the
  brand strip to the bottom of the window, its 48px head level with the shell bar (lake#567).
  **Expand over the page** widens it in place across everything but the rail, with a 200ms width
  transition (none while the edge is dragged, none under reduced motion), and **Back to the side**
  returns it; expanded, the conversation and its message box share one centred 880px column, and the
  panel stays above the shell bar (z-index 35) so its own head is never covered. **Open as a page**
  opens the same conversation on the Ask Orama home (`/`, about the page it came from). Before, this
  was a docked iframe of the server's Ask page. Its
  message box has the microphone (dictation, `components/dictation/Dictate.tsx`) and **From your
  phone**; a page can open it about something (`openAsk`, `lib/ask.ts`), as the transcript page
  does.
- **The Ask Orama home** (`/`, `pages/ask/AskHome.tsx`, lake#600) is where a signed-in person
  lands. It is the panel's conversation (`pages/ask/thread.ts`, the same `sessionStorage` entry)
  across the page's width, in one centred 880px column like the expanded panel, with the message box
  at the foot. Empty, it greets the person and offers up to four starting questions drawn from the
  workspace's datasets, projects and runs (`list_datasets`, `list_projects`, `list_runs`;
  `starterQuestions` in `pages/ask/home.ts`); pressing one asks it. While it is open the top bar's
  **Ask Orama** opens no panel: it puts the cursor in the message box, and a panel that was open
  closes. A link's `conversation`, `task` and `agent` go into the shared conversation and leave the
  address; `from` stays, shown as *About the page …* until dismissed. The operator and visitors
  keep the landing ([front-door.md](front-door.md)).
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
| `/` | `home/HomePage.tsx`, `ask/AskHome.tsx` | a signed-in person: Ask Orama, full page (lake#600); the operator and visitors: what lake is for and their next step ([front-door.md](front-door.md)); `/?ws=` (an old Command Center link) opens `/dashboard` | `viewer`; signed in, the Ask Orama capabilities below, `delete_ask_conversation` and engine `list_datasets`, `list_projects`, `list_runs` |
| `/dashboard` | `home/CommandCenter.tsx` | Home → Dashboard: the whole platform at a glance across the chosen workspaces (`?ws=`, the multi-select shared with Usage), what needs attention, each workspace's figures, recent activity, and a **Usage** button for the selection | `workspace_status`, `workspace_sql`, `list_transcripts`, `list_apps`, engine `list_projects`, `list_agents`, `list_runs`, `list_models`, `list_indexes` |
| `/sign-in`, `/sign-up`, `/mfa`, `/passkey-sign-in` | `account/SignInPage.tsx`, `SignUpPage.tsx`, `MfaPage.tsx`, `PasskeySignInPage.tsx` | lake's own sign-in and sign-up, or Clerk's (`account/ClerkSignIn.tsx`); see [identity.md](identity.md) | `sign_in_password`, `verify_mfa`, `start_oauth`, `sign_up_password`, `begin_passkey_sign_in`, `sign_in_passkey`, `enroll_mfa`, `disable_mfa` |
| `/login` | `account/LoginPage.tsx` | the operator-token form, posted to `POST /session` | none |
| `/settings`, `/sessions`, `/passkeys`, `/tokens` | `account/SettingsPage.tsx`, `SessionsPage.tsx`, `PasskeysPage.tsx`, `TokensPage.tsx` | profile, sessions and devices, passkeys, tokens ([settings.md](settings.md#personal-settings)) | `list_sessions`, `revoke_session`, `list_devices`, `revoke_device`, `begin_passkey_registration`, `register_passkey`, `create_token` |
| `/docs`, `/docs/:page` | `developer/DocsPage.tsx`, `docs.ts` | lake's guides (`docs/guide/*.md`) after rev-os's Docs reader: a left navigation grouped by section (folded behind **Contents** below 1024 px), a search box (titles and headings filtered at once from `list_docs`, the whole text from `list_docs {q}`, each result opening its heading), the guide with a **#** link on every heading and **Copy link**, **Previous** and **Next**, and **On this page** (a side column with the heading in view marked from 1280 px, a folded list above the guide below it); a `#anchor` in the address opens at that heading ([../api/web-app.md](../api/web-app.md#list_docs-and-get_doc)) | `list_docs`, `get_doc` |
| `/developer/api` | `developer/ApiPage.tsx` | a base-URL strip, then tabs Keys (make, list and revoke API keys), Quick start (curl, fetch and Python urllib with its own User-Agent, lake#589, against the viewer's origin) and Operations (every `/v1/openapi.json` operation, grouped, searchable, filtered by read or write, with its schemas and a curl line); downloads `openapi.json` ([../api/mcp.md](../api/mcp.md#developer--api-and-developer--mcp-lake561)) | `mcp_setup`, `create_token`, `list_sessions`, `revoke_session`, `GET /v1/openapi.json` |
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
| `/issues`, `/issue` | `repositories/IssuesPage.tsx`, `IssuePage.tsx`, `NewIssue.tsx`, `IssueParts.tsx` | issues and New issue; one issue, its conversation and its changes | `list_issues`, `list_issue_comments`, `issue_assignees`, `my_permissions`, `create_issue`, `update_issue`, `create_issue_comment` |
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
| `/search` | `search/SearchPage.tsx`, `lib/federatedPlaces.tsx`, `lib/federated.ts` | one query across the places the person ticks (`?q=&in=`): Repositories, Code (ranked, substring or regex, with its options), Datasets, Transcripts, Projects, Solutions (Orama's and the workspace's own, and the methodologies, each with its catalogue icon, lake#639), Agents, Models, Drives and Docs, searched side by side and grouped by place; workspace places in every workspace the person has, each hit naming its workspace when there are several and a workspace that failed named under the others (lake#633); a transcript shows the text around the match; the choice is remembered in this browser, and `?query=` still opens code search. The top bar's search runs the same places | `list_repositories`, `search`, `workspace_sql`, `list_transcripts`, `list_solutions`, `workspace_query` (`list_projects`, `list_methodologies`, `list_agents`, `list_models`), `search_drives`, `list_docs` (`q`) |

**Design, Train and Ship** (all under `/w/<workspace_id>/` except Deployments, Services and
Workspaces)

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/design/projects` | `design/ProjectsPage.tsx` | figures (projects, with pipelines, runs active), the Opportunities banner, projects with intent, pipelines and status (search, filter), New Project in a drawer | engine `list_projects`, `get_project` per project, `list_runs`, `create_project` |
| `/design/project?name=&tab=` | `design/ProjectPage.tsx`, `SolutionPanels.tsx` | one project: tabs Overview, Pipelines, Runs and Solution (`tab=`; Solution is the default when the project is a solution or has a foundation proposal), a side column with status, decision, dataset and actions; Build a Pipeline in a drawer; deletion in a dialog that asks for the typed name | `latest_agent_task`, `list_runs`, `compile_pipeline`, `create_pipeline`, `run_pipeline`, `confirm_solution`, `workspace_sql`; engine `get_project`, `get_graph`, `list_datasets`, `list_methodologies`, `approve_plan`, `delete_project`, solution reads and actions |
| `/design/pipeline?project=&graph=&step=` | `design/PipelinePage.tsx` | the pipeline graph in a large card, the selected step, the pipeline's facts and Approve/Check/Run/Deploy in the side column (approved, the Approve cell stays in place as a disabled **Approved** with its time, so the layout does not move; lake#578); the Project Agent, changes and runs below the graph | `render_pipeline`, `edit_pipeline`, `run_pipeline`, `agent_turn`, `get_agent_task`; engine `get_graph`, `list_datasets`, `list_runs`, `gate`, `deploy`, `approve_plan` |
| `/design/opportunities`, `/design/discover` | `design/OpportunitiesPage.tsx` | the scan banner, figures, recommendation cards each with Create project (confirmed in a dialog) and Dismiss, several picked into drafts on confirmation | `list_opportunities`, `create_projects_from_opportunities`, `dismiss_opportunity`, `agent_turn`, `get_agent_task` |
| `/design/solutions`, `/design/solution` | `design/SolutionsPage.tsx`, `SolutionPage.tsx`, `SolutionForm.tsx` | Orama's catalogue and the workspace's own solutions as cards marked **Orama** or **Yours** (search, a source filter, Orama's run state), **New solution** in a drawer; one of Orama's with its runs, **Start this solution** in a drawer and **Duplicate to edit**; one of your own with **Edit** (drawer), **Duplicate**, **Delete** (typed name) and **Start as a project** (drawer: a project with the solution's decision, and its first pipeline from the chosen methodology) ([solutions](../api/solutions.md), lake#597) | `list_solutions`, `get_solution`, `create_solution`, `update_solution`, `delete_solution`, `duplicate_solution`, `start_solution`, `compile_pipeline`, `list_runs`; engine `create_project`, `list_datasets`, `list_methodologies` |
| `/design/methodologies`, `/design/methodology` | `design/MethodologiesPage.tsx`, `MethodologyPage.tsx` | modelling methods as cards by family (search, filter); one methodology and the solutions that use it | engine `list_methodologies`, `list_solutions` |
| `/design/agents`, `/design/agent?id=` or `?key=`, `/design/agents/new` | `design/AgentsPage.tsx`, `AgentPage.tsx`, `AgentDrawers.tsx`, `agentActions.ts`, `train/FactoryPage.tsx` | where agents are designed (lake#653, was Agent Library; `/design/agent-library` redirects): Orama's agents marked **By Orama**, read-only, beside the workspace's own marked **Yours** (search, a By Orama / Yours filter), **New agent** in a drawer (purpose and prompt, the runtime's skills from `train/agent-skills.json`, vendored from agent-runtime, model, pipelines, work area, schedule) and the Agent Factory; one of Orama's with **Duplicate to edit**; one of your own with its definition, where it is used, its revisions with **Restore**, **Edit** (drawer), **Duplicate**, **Delete** (typed name) and **Run it** ([train-agents](../api/train-agents.md)) | `list_agent_library`, `list_agents`, `get_agent`, `create_agent`, `update_agent`, `delete_agent`, `list_agent_revisions`, `list_models`, `list_projects`, `list_agent_schedules`, `list_agent_launches`, `unschedule_agent`, `schedule_agent` |
| `/train/agents`, `/train/agent` | `train/AgentsPage.tsx`, `AgentPage.tsx` | training and improving the agents that learn (lake#653): only agents with something to train; one agent's training page (performance over time, versions and their data, promote and roll back, Launch experiment, the training schedule) and its runs and schedules ([train](train.md)) | `list_agents`, `get_agent`, `agent_training_status`, `set_training_schedule`, `update_agent`, `agent_turn`, `list_experiments`, `launch_agent`, `stop_agent_launch`, `list_agent_launches`, `list_agent_schedules`, `schedule_agent`, `unschedule_agent`, `usage_over_time` |
| `/design/agent-library` | `design/AgentLibraryPage.tsx` | the agents Orama provides, as cards (search, filter by availability) | `list_agent_library` |
| `/train/agents`, `/train/agent`, `/train/agents/new` | `train/AgentsPage.tsx`, `AgentPage.tsx`, `FactoryPage.tsx` | the space's agents, one agent, the Agent Factory | `schedule_agent`, `unschedule_agent`, `launch_agent`, `stop_agent_launch`, `list_agent_launches`, `list_agent_schedules`, `approve_agent_proposal`; engine `list_agents`, `create_agent`, `get_agent`, `update_agent`, `delete_agent`, `agent_training_status` |
| `/train/data`, `/train/dataset`, `/train/query` | `train/DataPage.tsx`, `DatasetPage.tsx`, `QueryPage.tsx` | load data, connections, datasets, storage; one dataset, and Delete dataset ([deletion.md](deletion.md)); read-only SQL | `delete_dataset`, `list_stores`, `create_store`, `delete_store`, `store_connection`, `list_services`, `connect_service`, `workspace_sql`; engine connection, dataset and generator calls |
| `/train/agents`, `/train/agent`, `/train/agents/new` | `train/AgentsPage.tsx`, `AgentPage.tsx`, `FactoryPage.tsx` | the account's agents, one agent, the Agent Factory | `schedule_agent`, `unschedule_agent`, `launch_agent`, `stop_agent_launch`, `list_agent_launches`, `list_agent_schedules`, `approve_agent_proposal`; engine `list_agents`, `create_agent`, `get_agent`, `update_agent`, `delete_agent`, `agent_training_status` |
| `/train/data`, `/train/dataset`, `/train/query` | `train/DataPage.tsx`, `DatasetPage.tsx`, `QueryPage.tsx` | load data, connections, datasets, storage; one dataset; read-only SQL | `list_stores`, `create_store`, `delete_store`, `store_connection`, `list_services`, `connect_service`, `workspace_sql`; engine connection, dataset and generator calls |
| `/drives`, `/drives/drive?id=&folder=` | `drives/DrivesPage.tsx`, `DrivePage.tsx`, `NewDrive.tsx`, `FolderPicker.tsx`, `ShortcutDialog.tsx` | Data → Drives: the workspace's drives and recent files; one drive as a file manager over its storage (uploads of files and folders, selection, drag to move, shortcuts, search of the whole drive, drive settings) ([drives.md](drives.md)) | `list_drives`, `get_drive`, `create_drive`, `rename_drive`, `delete_drive`, `recent_drive_entries`, `search_drives`, the entry and upload capabilities, or `drive_browse` and the drive_* writes for a repository drive |
| `/design/opportunities`, `/design/discover` | `design/OpportunitiesPage.tsx` | the scan banner, figures, recommendation cards each with Create project (confirmed in a dialog) and Dismiss, several picked into drafts on confirmation; each card carries the recommended solution's icon, or the methodology's (lake#639) | `list_opportunities`, `create_projects_from_opportunities`, `dismiss_opportunity`, `agent_turn`, `get_agent_task` |
| `/design/solutions`, `/design/solution` | `design/SolutionsPage.tsx`, `SolutionPage.tsx`, `SolutionForm.tsx` | Orama's catalogue and the workspace's own solutions as cards marked **Orama** or **Yours** (search, a source filter, Orama's run state), **New solution** in a drawer; one of Orama's with its runs, **Start this solution** in a drawer and **Duplicate to edit**; one of your own with **Edit** (drawer), **Duplicate**, **Delete** (typed name) and **Start as a project** (drawer: a project with the solution's decision, and its first pipeline from the chosen methodology) ([solutions](../api/solutions.md), lake#597); every card and the page header carry the solution's icon, and its methodologies are listed with theirs ([catalogue-icons.md](catalogue-icons.md), lake#639) | `list_solutions`, `get_solution`, `create_solution`, `update_solution`, `delete_solution`, `duplicate_solution`, `start_solution`, `compile_pipeline`, `list_runs`; engine `create_project`, `list_datasets`, `list_methodologies` |
| `/design/methodologies`, `/design/methodology` | `design/MethodologiesPage.tsx`, `MethodologyPage.tsx` | modelling methods as cards by family (search, filter); one methodology and the solutions that use it; each with its icon ([catalogue-icons.md](catalogue-icons.md), lake#639) | engine `list_methodologies`, `list_solutions` |
| `/design/icons` | `design/IconsPage.tsx` | Catalogue Icons: every solution and methodology icon at 20, 32 and 64 px and the fallbacks, for review; not in the rail, open to everyone (lake#639) | none |
| `/design/agent-library` | `design/AgentLibraryPage.tsx` | the agents Orama provides, as cards (search, filter by availability) | `list_agent_library` |
| `/train/agents`, `/train/agent`, `/train/agents/new` | `train/AgentsPage.tsx`, `AgentPage.tsx`, `FactoryPage.tsx` | the space's agents, one agent, the Agent Factory | `schedule_agent`, `unschedule_agent`, `launch_agent`, `stop_agent_launch`, `list_agent_launches`, `list_agent_schedules`, `approve_agent_proposal`; engine `list_agents`, `create_agent`, `get_agent`, `update_agent`, `delete_agent`, `agent_training_status` |
| `/train/data`, `/train/dataset`, `/train/query` | `train/DataPage.tsx`, `DatasetPage.tsx`, `QueryPage.tsx` | load data and datasets; one dataset, and Delete dataset ([deletion.md](deletion.md)); read-only SQL. An old `?browse=` or `#connections` link goes to Connections, `#storage` to Drives → Storage | `delete_dataset`, `workspace_sql`; engine dataset, generator, `list_connections` and `create_dataset_from_query` calls |
| `/connections`, `/connections/connection?name=` | `connections/ConnectionsPage.tsx`, `ConnectionPage.tsx`, `forms.tsx`, `connections.ts` | Data → Connections: figures, the list with kind, host, service, status and created date, Add connection, Connect One of Your Services, typed delete; one connection's tables and columns with Load into a dataset ([connections.md](connections.md)) | `list_services`, `connect_service`; engine `list_connections`, `create_connection`, `delete_connection`, `list_source_tables`, `create_dataset_from_query` |
| `/drives`, `/drives?tab=storage`, `/drives/drive?id=&folder=`, `/storage` | `drives/DrivesPage.tsx`, `StorageTab.tsx`, `DrivePage.tsx`, `NewDrive.tsx`, `FolderPicker.tsx`, `ShortcutDialog.tsx`, `StorageRedirect.tsx` | Data → Drives: the workspace's drives and recent files (a drive of any of four kinds, a storage drive browsing a bucket or filesystem in place, lake#656); the Storage tab with the workspace's buckets, filesystems and volumes (moved from Datasets; `/storage` leads there); one drive as a file manager over its storage (uploads of files and folders, selection, drag to move, shortcuts, search of the whole drive, drive settings) ([drives.md](drives.md)) | `list_drives`, `get_drive`, `create_drive`, `rename_drive`, `delete_drive`, `recent_drive_entries`, `search_drives`, the entry and upload capabilities, or `drive_browse` and the drive_* writes for a repository drive; `list_stores`, `list_store_providers`, `create_store`, `delete_store`, `store_connection`, `usage_over_time` on the Storage tab |
| `/transcripts`, `/transcript?id=` | `transcripts/TranscriptsPage.tsx`, `TranscriptPage.tsx`, `NewTranscript.tsx` | Data → Transcripts: record, upload or paste a transcript; search, read, edit, copy, delete, Ask Orama about it ([transcripts.md](transcripts.md)) | `list_transcripts`, `get_transcript`, `create_transcript`, `update_transcript`, `delete_transcript`, `transcribe` |
| `/train/runs`, `/train/run` | `train/RunsPage.tsx`, `RunPage.tsx` | pipeline runs; one run's report, lineage, log and trace, and Delete once it finished | `delete_run`; engine `list_runs`, `list_studies`, `get_run`, `get_run_report`, `list_traces` |
| `/train/experiments`, `/train/experiment`, `/train/experiment/run`, `/train/experiments/compare` | `experiments/*.tsx` | experiments and runs with live charts ([experiments.md](experiments.md)) | see experiments.md |
| `/train/models`, `/train/model` | `train/ModelsPage.tsx`, `ModelPage.tsx`, `NewModel.tsx`, `models.ts`, `modelParts.tsx` | training each model: its versions, held-out scores over versions, comparing versions, what it learned from, its pipeline steps, Retrain and Promote to Ship; New model (train one or bring one), and a standalone model's New version, Move into a project and Delete ([train.md](train.md#a-standalone-model)) | `run_pipeline`, `train_model`, `retrain_model`, `register_model`, `load_drive_model`, `move_model_to_project`, `delete_model`, `resolve_model_file`, `recent_drive_entries`; engine `list_models`, `list_studies`, `list_datasets`, `list_methodologies`, `get_run_report`, `get_graph`, `expose_model`, `unexpose_model` |
| `/train/streams`, `/train/stream` | `streams/StreamsPage.tsx`, `TopicPage.tsx` | topics and messages ([streams.md](streams.md)) | see streams.md |
| `/ship/models`, `/ship/model` | `ship/ModelsPage.tsx`, `ModelPage.tsx` | serving each model: the version published, inference with Try It, access (predict keys, origins), destinations and monitoring ([ship.md](ship.md)), and Delete a version | `delete_model`, `predict`, `model_traffic`, `list_sessions`, `create_token`; engine `list_models`, `expose_model`, `unexpose_model` |
| `/ship/indexes`, `/ship/index` | `ship/IndexesPage.tsx`, `IndexPage.tsx` | search indexes, one index, search it, delete it | `delete_index`, `search_index`, `create_token`; engine `list_indexes`, `create_index`, `get_index`, `expose_index`, `unexpose_index` |
| `/ship/packages` | `apps/PackagesPage.tsx` | the instance registry, the space's images and builds | `list_packages`, `list_image_repositories`, `list_builds`, `receive_build_events` |
| `/ship/models`, `/ship/model` | `ship/ModelsPage.tsx`, `ModelPage.tsx` | serving each model: the version published, inference with Try It, access (predict keys, origins), destinations and monitoring ([ship.md](ship.md)) | `predict`, `model_traffic`, `list_sessions`, `create_token`; engine `list_models`, `expose_model`, `unexpose_model` |
| `/ship/indexes`, `/ship/index` | `ship/IndexesPage.tsx`, `IndexPage.tsx` | search indexes, one index, search it | `search_index`, `create_token`; engine `list_indexes`, `create_index`, `get_index`, `expose_index`, `unexpose_index` |
| `/ship/packages` | `apps/PackagesPage.tsx` | the instance registry, the account's images and builds | `list_packages`, `list_image_repositories`, `list_builds`, `receive_build_events` |
| `/ship/apps`, `/ship/app` | `apps/AppsPage.tsx`, `AppPage.tsx` | apps, ApplicationAgent, one app ([apps.md](apps.md)) | see apps.md |
| `/deployments` | `apps/DeploymentsPage.tsx` | every environment across readable repositories | `server_info` (whether Deployments are set up, lake#605), `list_repositories`, `list_environments` |
| `/services` | `ship/ServicesPage.tsx` | the account's databases, streams and engines | `list_services`, `list_service_kinds`, `create_service`, `delete_service`, `service_connection` |
| `/workspaces` | `workspaces/WorkspacesPage.tsx` | every workspace the person can reach, grouped by owner as the picker groups them, with phase and storage; New workspace (`?new=1`, asking the owner), Rename and deletion in dialogs ([workspace-selection.md](workspace-selection.md#the-workspaces-page)) | `list_workspaces`, `create_workspace`, `update_workspace`, `delete_workspace` |
| `/ask`, `/w/<id>/ask` | `ask/AskRedirect.tsx` | the old Ask Orama page's address: opens the home (`/`) with its query, less `customer_id`; `/w/<id>/ask` first makes `<id>` the current workspace ([../api/ask-orama.md](../api/ask-orama.md)) | none; the home calls `send_ask_message`, `settle_ask_turn`, `confirm_ask_proposal`, `get_ask_conversation`, `get_agent_task`, `list_agent_library` |

**Organization and administration** ([settings.md](settings.md), [identity.md](identity.md))

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/organizations` | `organizations/OrganizationsPage.tsx` | organizations, invitations for you, create, convert | `list_organizations`, `list_invitations`, `join_organization`, `create_organization_space`, `convert_personal_space`, `get_customer` |
| `/organization?id=&tab=` | `organizations/OrganizationPage.tsx` | one organization's tabs; Delete organization for an owner, refused while it owns a workspace; Revoke on a pending invitation | `delete_organization`, `revoke_invitation`, `get_organization`, `list_teams`, `create_team`, `list_memberships`, `list_invitations`, `create_invitation`, `entity_effective_access`, `list_sso_providers`, `register_sso_provider`, `list_applications`, `metering_usage`, `audit` |
| `/team`, `/teams`, `/member`, `/members`, `/invitations`, `/grants` | `organizations/TeamPage.tsx`, `TeamsPage.tsx`, `MemberPage.tsx`, `MembersPage.tsx`, `InvitationsPage.tsx`, `GrantsPage.tsx` | teams, memberships, invitations (Revoke), direct grants (Revoke) | `revoke_invitation`, `revoke_grant`, `get_team`, `list_teams`, `get_membership`, `set_member_role`, `remove_member`, `list_memberships`, `list_invitations`, `list_grants`, `get_object` |
| `/access`, `/repository/access`, `/organization/access`, `/team/access` | `organizations/AccessPage.tsx` | effective access to one object, each source linked to its direct grants | `effective_access`, `entity_effective_access`, `get_object` |
| `/sso` | `organizations/SsoPage.tsx` | SSO providers, federation issuers, a SCIM log | `list_sso_providers`, `list_federation_issuers`, `list_sso_provisioning_logs` |
| `/customers` | `admin/CustomersPage.tsx` | every account | `list_customers` |
| `/applications`, `/application`, `/application/users`, `/application/organizations`, `/application/events` | `admin/Application*.tsx` | identity applications ([identity.md](identity.md#application-identity)) | `list_applications`, `register_application`, `get_application`, `list_application_users`, `suspend_application_user`, `list_application_organizations`, `create_application_organization`, `delete_application_organization`, `list_application_webhook_events`, `create_application_webhook` |
| `/usage?ws=&range=&dimension=` | `usage/UsagePage.tsx` | Usage: every dimension over time across the chosen workspaces, against the period before, charted by workspace or kind, with limits and CSV ([usage.md](usage.md), lake#564) | `usage_over_time`, `usage_limits`, `set_usage_limit` |
| `/usage/accounts` | `usage/AccountsPage.tsx` | the operator's usage across every account | `list_customers`, `usage_over_time` |
| `/billing?customer_id=` | `billing/BillingPage.tsx` | Billing, only where the lake bills: this month's usage × rate = cost so far and the invoices, with no plan to choose; the operator issues an invoice ([billing.md](billing.md), lake#564, lake#635) | `billing_account`, `price_list`, `bill_preview`, `list_invoices`, `issue_invoice` |
| `/billing/invoices/:invoiceId` | `billing/InvoicePage.tsx` | one invoice, printable; the operator records a payment or voids it | `get_invoice`, `record_invoice_payment`, `void_invoice` |
| `/billing/plans` | `billing/PriceListPage.tsx` | the operator's price list editor: one rate per usage dimension, published from a month | `price_list`, `set_price_list` |
| `/metering?customer_id=` | `admin/MeteringPage.tsx` | Repository Usage by Hour: repository transfer and build time, the storage limit | `metering_usage`, `set_storage_quota` |
| `/secret-mounts`, `/secrets`, `/encryption-keys` | `admin/SecretMountsPage.tsx`, `SecretsPage.tsx`, `EncryptionKeysPage.tsx` | secret mounts and New mount; a mount's secrets and New secret; a secret's versions, New version, a deliberate Reveal, delete, undelete and a typed-confirmation destroy; keys with New key, Rotate and Try it ([secrets.md](secrets.md)); Delete a mount ([deletion.md](deletion.md)); never key material | `delete_secret_mount`, `list_secret_mounts`, `create_secret_mount`, `get_secret_mount`, `list_secrets`, `put_secret`, `read_secret`, `secret_metadata`, `delete_secret_versions`, `undelete_secret_versions`, `destroy_secret_versions`, `list_encryption_keys`, `create_encryption_key`, `derive_encryption_key`, `rotate_encryption_key`, `encrypt`, `decrypt`, `rewrap`, `sign`, `verify`, `hmac` |
| `/activity`, `/audit` | `admin/AuditPage.tsx` | the person's own events; the operator's whole log | `my_audit`, `audit`, `transparency_log_configuration` |
| `/transparency?identity=` | `admin/TransparencyPage.tsx` | the public checkpoint-verified scan for one TAAP identity | `transparency_identity_scan` |
| any other path | `home/NotFoundPage.tsx` | *not found* in lake's words | none |

## Rules the pages keep

- **Public reads open signed out.** Public repositories, public models, the Docs, the transparency
  monitor and share links open without a session. A refused repository read for a visitor goes to
  `/sign-in?next=…`, so a private repository's existence is not disclosed
  (`RepositoryError` in `pages/repositories/forge.tsx`).
- **Secrets are shown only when asked for.** The secrets pages list metadata; a secret's value is
  read only by a deliberate Reveal, held in that page's memory, masked until shown and never put in a
  URL, a log or browser storage ([secrets.md](secrets.md)). Key material is never returned. A new
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
| `catalogue-icons/` | 12 PNGs of the web app (not the server pages): the catalogue icons on Solutions, Methodologies, a solution, a methodology, Opportunities, Search and the gallery, at 1440 and 390 px, light and dark, from `frontend/checks/catalogue-icons.mjs` ([catalogue-icons.md](catalogue-icons.md)) |

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
