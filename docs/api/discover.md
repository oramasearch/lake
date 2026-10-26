# Design → Opportunities

**Updated 2026-10-25 (lake#1195): a scan survives leaving the page.** Before, the page kept the
scan's task only in its address (`?task=`), and a finished scan was saved into Opportunities only when
`list_opportunities` was read with that `task_id`: leave the page while the scan ran, and both the
progress and the results were lost (lake-dev, 2026-10-25). Now:

- **Lake saves a finished scan itself.** When Auto-Suggest Agent publishes its `result` for a
  `discover` task (`publish_agent_event`), lake saves the scan's Opportunities into the workspace Zetta
  at once, whether or not anyone is reading. It uses `settle_discover`, the one save function every
  reader (`get_agent_task`, `receive_agent_events`, `list_opportunities`) already settles with, so a
  scan is still saved once (each record's key is derived from the scan and its rank). A workspace that
  cannot take them at that moment is retried by the next `list_opportunities`, which saves the
  workspace's newest finished scan when its Opportunities are not listed yet.
- **`list_opportunities` answers `last_scan`**: the workspace's newest scan, whoever started it,
  `{task_id, status, started_at, finished_at, message}` (`status` is `pending`, `running`, `succeeded`
  or `failed`; `started_at` is when it was asked for; `finished_at` is null while it runs; `message` is
  the agent's last word or lake's notice), or `null` before the first scan. We chose this over a
  `discover` subject on `latest_agent_task`: it is one read the page already makes, it works the same
  over REST and MCP, and it lets every member of the workspace see the scan (Opportunities belong to
  the workspace; `latest_agent_task` follows only the caller's own task). `task_id` is still accepted
  and still harmless.
- **One scan of a workspace at a time.** `agent_turn {agent: "agent-autosuggest", kind: "discover"}`
  while that workspace's newest scan is pending or running does not start another: it answers that
  scan's task with `joined: true`.
- **The page** reads `last_scan` with the Opportunities, again every 3 seconds while the scan runs,
  and has no `?task=`. It says *Scanning… started 2m ago* (with the agent's last message when there is
  one) from any tab or device, the Scan button says *Scanning…* and is disabled, and the results appear
  when it finishes. A failed latest scan says *The scan did not finish* with its message.

Verify: `Stores/WorkspaceRecordStores.OpportunitiesReportTheLatestScanWithoutItsTaskId` (memory and
PostgreSQL: no scan, pending, running, finished and listed without `task_id`),
`HttpServer/HttpResponseIsolation.DiscoverOpportunitiesScansAndCreatesProjects` (the running scan in
`last_scan`, a second scan joins it, the Opportunities saved as the agent's result settles the task,
then listed without `task_id`), the web app's `npm test` (`scanState`, `scanPollInterval`, and the page
reading no `?task=`), and `frontend/checks/opportunities-scan.mjs` (the page in a browser with the
running scan, then its results, and no `?task=`).

**Updated 2026-10-25 (lake#1153): a clean table gets its Opportunity.** Auto-Suggest Agent plans every
dataset with `plan_features`, which now reads the cutoff a table's rows were observed at from its
snapshot column (`as_of`, `observed_at`, ...) instead of declining every table with a date as
*transform first*, and names the Lake capability that performs any preparation that remains
([feature-planning.md](feature-planning.md)). Auto-Suggest Agent's side (prompt v5, a *transform first*
that the feature plans contradict becomes an Opportunity with its preparation in `required_first`)
is [agent-autosuggest](https://github.com/oramasearch/agent-autosuggest)'s.

**Updated 2026-10-03 (lake#376): Opportunities live in the workspace Zetta of the workspace their
scan ran in.** A finished scan is saved, once, as records of the `lake_opportunities` collection in
that workspace's Zetta ([workspace records](../architecture/workspace-records.md)), sealed and found
by the account. Each record's key is derived from the scan and its rank, so however many readers or
replicas settle the scan, it is saved once. `list_opportunities`, `dismiss_opportunity` and
`create_projects_from_opportunities {opportunity_ids}` take `workspace_id` (required, as for every
workspace capability) and act on that workspace's Opportunities only: another workspace of the same
account lists none of them and answers `not_found` for their ids, and a missing, malformed or unknown
`workspace_id` is `invalid_input` at `/workspace_id`. A status change (`dismissed`, `project`) is a
write conditional on the revision read, reread and retried when another write landed first. A task
that names no workspace has nowhere to keep its Opportunities and saves none; a workspace that is not
ready answers `workspace_not_ready`; deleting a workspace deletes its Opportunities with its Zetta.
The page reads the Opportunities and the methodology registry concurrently (two calls, one round).
Lake no longer writes or reads the `opportunities` table; the operator's one-shot
`copy_workspace_records {"collection": "opportunities"}` moved the old rows (migration
`0046_opportunity_workspaces`), and migration `0070_drop_moved_tables` drops the table (lake#311). Verify: `Stores/WorkspaceRecordStores.OpportunitiesStayInTheWorkspaceTheirScanRanIn`,
`WorkspaceRecords.AnOpportunityStatusChangeRereadsAfterAConcurrentWrite`.

**Updated 2026-09-27 (lake#269):** Discover became **Design → Opportunities** (`/design/opportunities`;
the web app also opens `/design/discover`). *Scan my workspace* is a background Auto-Suggest Agent turn
(`agent_turn`, kind `discover`); the page follows it and the Opportunities it recommends (and the
ones it declines, *Not yet*) are **kept** as rows of `opportunities`, across scans. Each can become a
project draft (`create_projects_from_opportunities {opportunity_ids}`, on confirmation) or be dismissed
(`dismiss_opportunity`). `list_opportunities {task_id?}` reads them over REST and MCP. Each names the
catalogue solution it maps to; the catalogue itself in lake is lake#270.

**A project made from an Opportunity gets its pipeline.** Creating the project starts a Project
Agent task in the background (kind `build:<project>`), briefed with the Opportunity: its intent, what
it changes, why the data supports it, its catalogue solution and methodology. The agent reads the
workspace and answers a design (the dataset, the label, the features, the model and the hold-out),
never a change. The project's page says *The Project Agent is designing this project's pipeline…*
while it works. Once it is done, reading the task (the project page, `get_agent_task`,
`receive_agent_events`) applies the design, once: Zetta compiles a pipeline from a project's intent
when the project is created, so the project's first unapproved pipeline is pointed at the design's
table (`dataset`, else the first of `training_sources`), its other connect steps removed, and its
model step given the design's `target` (label) and `algo` (model kind). A project with no pipeline
is compiled from the design first, exactly as *Build a Pipeline* does. A pipeline that already reads
the design's table is left alone. A design that cannot be compiled, or a failed task, is
said on the page, with *Build a Pipeline* still there. `create_projects_from_opportunities` answers
the tasks as `builds`. Verify: `HttpResponseIsolation.DiscoverOpportunitiesScansAndCreatesProjects`
(`agent_turn` kind `discover`, `list_opportunities`, `create_projects_from_opportunities`,
`dismiss_opportunity`, and `latest_agent_task` applying the Project Agent's design once).

**Each project gets the methodology its Opportunity needs (lake#289).** lake resolves the Opportunity's methodology against the engine's `list_methodologies`: an `M-n` written in its `methodology`, else the methodology that serves its catalogue `solution`.

- **It runs:** the project is created without an intent, so the engine compiles no skeleton, and its pipeline is compiled with `compile_intent {methodology}`. The Project Agent's design is applied to it.
- **It does not run yet:** the project is created with no pipeline, and its description says so. It is answered in `waiting: [{project, methodology, name, issue, message}]`, and no build starts.
- **`settle_build`** uses the design's methodology, else the Opportunity's. A design whose methodology does not run is refused with `not_supported`, shown on the project page. A pipeline of another methodology's shape (the old churn skeleton) gets a new pipeline compiled with the right one instead of being repointed.
- **On the page:** each Opportunity card says *Runs today: <name> (M-n)* or *Needs M-n, not supported yet (zetta#…)*. The project page's pipelines show their methodology, or *Trains nothing yet*. A pipeline that cannot train says so at the top of its page. *Build a Pipeline* offers the methodologies that run.
- Verify: `WorkspaceAgents` in `tests/test_workspaces.cpp` (lake#289 block).

**The latest scan is the recommendation (lake#287).** *Recommended* lists the newest scan's
Opportunities. What earlier scans recommended and nobody acted on folds under *Earlier scans (N)* in the
same form, where it can still be picked or dismissed; *Not yet* is the newest scan's. Nothing is
deleted or rewritten: the grouping is the web app's, decided when the page is drawn, and everything
a person acted on (made into a project, dismissed) keeps its own section. `list_opportunities` still
returns every row, each with its `task_id`, which is what the grouping reads.

# Discover Opportunities

**Status: implemented 2026-09-27 (lake#237, epic lake#228).** Design's *Discover Opportunities*
(`/design/discover`, linked beside *New Project*), backed by the Agent Library's Auto-Suggest Agent
([`oramasearch/agent-autosuggest`](https://github.com/oramasearch/agent-autosuggest)). It follows
Studio's discovery journey (orama-design `src/studio/discovery/`).

1. **Scan my workspace** sends one `agent_turn` (kind `discover`) to
   `agent-autosuggest`. Auto-Suggest Agent builds the **Workspace Profile** itself from the data
   the workspace holds (`list_datasets`, then `get_dataset` for up to twelve: names and columns),
   with the turn's read-only key. The same key lets it read the engine's methodologies and
   solutions and the account's Ship Apps and image builds (`list_apps`, `get_app`,
   `receive_app_events`, `list_builds`, `get_build`, `receive_build_events`; lake#795), and change
   nothing. The key's exact contents are in [agent-launches.md](agent-launches.md#turns).
2. **Recommended Opportunities**: Auto-Suggest Agent's `suggested` entries, each with its intent, solution
   id, methodology, why this data supports it, what must come first, and what it changes; **Not Yet**:
   its `declined` entries with the reason and *capture it first* or *cannot be done*.
3. **Create project drafts** (`create_projects_from_opportunities`) needs the person's explicit
   confirmation. Each selected Opportunity becomes a project whose intent is the Opportunity's and
   whose description carries its **Source Opportunity** (solution, methodology, evidence). Nothing
   runs until a plan is approved.

When Auto-Suggest Agent is not running on the lake (`LAKE_AGENT_ENDPOINTS`), the page says so and offers
nothing behind a mock. A scan that fails says why.

Verify: `HttpServer/HttpResponseIsolation.DiscoverOpportunitiesScansAndCreatesProjects`, and
`AgentRuns.AutoSuggestDiscoverReadsAppsAndBuildsWithItsTaskCredential` for the turn key.
