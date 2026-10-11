# Train and Data Pages

The pages in `frontend/src/pages/train/` (lake#561): Agents, one Agent, the Agent Factory, Runs, one
Run, and the Data group's Datasets page (`/train/data`) with one Dataset and Query. They follow
the page rules in [web-app.md](web-app.md): a header with the purpose and the primary action,
figures from the reads already made, a toolbar over each list, creation in a drawer or dialog,
details as a split with a side column of facts and actions, and confirmations that name what
happens. A header action that needs the workspace (Load data, New agent) shows only once
`workspace_status` answers ready (`useWorkspaceReady` in `workspace.tsx`).

## What each page shows

| Page | Path | Reads | Layout |
|---|---|---|---|
| Datasets | `/w/<id>/train/data` | `system.datasets` through `workspace_sql`, `list_connections`, `list_services`, `list_stores`, `list_store_providers`; `list_source_tables` with `?browse=` | Figures (datasets, run outputs, connections, stores); a searchable table of datasets filtered to Datasets (the default) or Run outputs, each with its database, kind and a Query link; "Load data" opens a drawer with Upload (`upload_dataset`), Generate (`generate_dataset`, `list_dataset_generators`) and From a connection (`create_dataset_from_query`); the Connections card (table, Browse tables, Remove behind a confirmation, Connect One of Your Services, Add connection in a drawer); the Storage card (status chips, polled every 3 seconds while a store changes, New storage in a dialog, the connection in a dialog, delete in a dialog that needs the store's name typed). `?loaded=…` shows the upload's result as a banner; `?error=` as a warning |
| Dataset | `/w/<id>/train/dataset?database=…&name=…` | `get_dataset`, `analyze_dataset`, the first 20 rows through `workspace_sql` | Figures (rows, size, columns, copies); a split: tabs Columns, First Rows and Partitions on the left; the facts and the actions (Query it, Keep durable with `promote_dataset` when a partition has one copy) on the right |
| Query | `/w/<id>/train/query?statement=…&database=…` | `workspace_sql` | An editor card (⌘/Ctrl+Enter runs it) and a results card with the first 500 rows; the statement and database stay in the URL |
| Agents | `/w/<id>/train/agents` | `list_agents`, `list_agent_library` | Figures (agents, run on their own, conversation only, frontier model); a searchable card grid filtered by engagement, each card with its purpose, trigger or Conversation chip, model and skills; "New agent" opens a drawer (`create_agent`, then `schedule_agent` for a frontier agent's trigger); "Build with the Agent Factory" goes to the factory; the Agent Library as cards |
| Agent | `/w/<id>/train/agent?id=…` | `get_agent`, `list_projects`, `list_agent_launches`, `list_agent_schedules`, `agent_training_status` | A split: tabs Overview (instructions, goal, Pipelines granted and grantable through `update_agent`), Unattended Runs (Run now with `launch_agent`, Stop with `stop_agent_launch`, polled every 5 seconds while one starts), Schedules for a frontier agent (Add schedule in a dialog, Remove), and Training when the read answers; the facts and actions on the right; delete in a dialog that needs the agent's name typed, which also removes its schedules |
| Agent Factory | `/w/<id>/train/agents/new?conversation=…&task=…` | `get_ask_conversation`, `get_agent_task` | A conversation card with AgentCreator (bubbles, example prompts when empty, the working notice, the composer) beside the proposal card with "Approve and create" (`approve_agent_proposal` with the stored digest), or How It Works before there is one |
| Runs | `/w/<id>/train/runs` | `list_runs`, `list_studies` | Figures (runs, succeeded, failed, in progress); a searchable table filtered by status, with status chips, relative start times and Report or Why it failed, and Charts |
| Run | `/w/<id>/train/run?id=…` | `get_run` (every 15 seconds while unfinished), `get_run_report` once finished, `list_traces` | A split: notices, the report (the held-out score as a figure with its caveat, what it learned from, drivers, at risk, caveats, missing, the raw report), lineage, outputs, log and trace on the left; the facts on the right; Training charts and Workspace streams in the header |

`workspace.tsx` holds the calls and helpers the pages share (`wq`, `wa`, `useWq`, `useSpaceRead`,
`timeAgo`, `useWorkspaceReady`); `agents.tsx` what the agent pages say about an agent and the Agent
Library; `datasets.tsx` the lean dataset list and `ObjectTable`; `RunsPage.tsx` the run and experiment
helpers the Run and Models pages share (`useExperimentRefs`, `TrialSubject`).

## How to verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` from `frontend/` (the build also runs
`check-effects` and `check-list-errors`). To look at the pages without a lake, serve them with
`npx vite` and answer `/v1/*` from Playwright's `page.route` with fixtures for the reads above
(`workspace_query` answers by its `capability`). `tools/e2e/storage.mjs` drives the Storage card
on a live lake.

## How to change

Keep the reads, actions, URL parameters and guards above: the typed name before `delete_store` and
`delete_agent`, the 16 MiB upload limit and the dataset name pattern, the whole-number row limit of
a pull, the 1–50 clamp of a generator's variants, the consent-required message of a governed
workspace, the report asked for only after a run finishes, and never `path` on
`generate_dataset`.
