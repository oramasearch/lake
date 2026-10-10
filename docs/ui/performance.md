# Performance

**What is measured now (lake#561).** The console is a single-page app, so lake no longer renders
pages. A page's time is the app's bundle, loaded once and cached, plus the capability calls the
page makes. Lake answers a page route with the embedded `index.html` and no workspace call
(recorded under the route `web_app`). What is measured is therefore:

- **capability latency**: each call's `duration_ms` in its JSON envelope, its `Server-Timing`
  header (below), and the `lake_http_request_duration_seconds{method,route,status}` histogram on
  `/metrics` ([../serving/telemetry.md](../serving/telemetry.md));
- **the bundle**: its size, how it is split, and how it is cached (below).

**What no longer applies.** The 100 ms p95 page rule, the per-route workspace call and round
budgets, the 500 ms regression guard and the deferred panels measured the server-rendered HTML
routes (`PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget`, lake#446). Those
routes now answer `index.html`, so their budgets measure nothing; the deferred panels are gone
because the app reads `list_agents` and the dataset list itself, after the page is on screen. A
page's calls run concurrently by default (each `useRead` starts on its own), and a slow one fills
in its own panel instead of holding up the page. No per-page budget for capability calls exists
yet.

## Reading `Server-Timing`

Every `/v1/<capability>`, `/w/<workspace_id>/v1/<capability>` and `/mcp` response carries a
`Server-Timing` header (`server_timing_of` in `src/api/http_server.cpp`). The browser's Network
panel shows these entries for each call the app makes:

- `total` is lake's server time from the first request middleware until the response is ready,
  including workspace resolution, authentication and the capability itself.
- `workspace` is the accumulated duration of calls to the customer's workspace. Its description
  gives the number of calls, for example `3 calls`.
- `workspace_rounds` counts sequential outbound rounds. Concurrent reads started before any
  earlier read completes belong to the same round; a follow-up after completion starts another.

- `zetta_<capability>` is one workspace call, named by the Zetta capability it called
  (`zetta_list_models`, `zetta_sql` for a `workspace_sql` read), with its own duration. There is one
  entry per call, in completion order, so a slow call names the engine read it waited on.
- `store` (lake#527) is the request's traffic to a switched workspace's repository store (its
  liquid store in the workspace Zetta, [workspace-store.md](../architecture/workspace-store.md)):
  the summed duration and, as its description, the number of object requests. Then one
  `store_<kind>` entry per kind sent (`put`, `put_if_absent`, `patch`, `barrier`, `get`,
  `ranged_get`, `list`, `delete`, `lease_acquire`, `lease_release`, `create_bucket`), its duration
  and count. They appear only when the request reached a workspace store, so a repository write such as
  `commit_repository_files` names its store requests.

Workspace event-bus reads for apps, builds and agent tasks use the same timing path as ordinary
workspace capabilities.

An example header is:

```text
Server-Timing: total;dur=47.418, workspace;dur=30.731;desc="1 calls", workspace_rounds;desc="1 rounds", zetta_list_models;dur=30.731
```

Concurrent workspace calls contribute their individual durations to `workspace`, so that value
can exceed `total`. The count exposes accidental fan-out even when the calls overlap.

## The bundle

`npm run build` in `frontend/` (Vite 6.4.4) writes `dist/`; lake embeds it
([web-app.md](web-app.md)).

- **Code-split pages.** Every screen is a dynamic import (`lazyPage`), so each page is its own chunk
  and loads the first time it is opened. Code shared by a group is its own chunk (the uPlot chart
  code loads only with the Experiments pages).
- **Immutable hashed assets.** Every file but `index.html` has a content hash in its name under
  `/ui/` and is served `Cache-Control: public, max-age=31536000, immutable`. `index.html` is
  `no-cache`, so a deploy is picked up on the next load and unchanged chunks stay cached. A tab
  that outlives a deploy reloads once when it asks for a chunk the new build no longer has.
- **No third-party requests.** Fonts, scripts and styles all come from lake.

Measured from the `npm run build` output at `ec0a7b2` (2026-10-10; sizes as Vite reports them,
gzip in brackets):

| What | Size |
|---|---|
| Files in `dist/ui/` | 132: 124 scripts, 3 stylesheets, 5 fonts |
| Entry script `index-*.js` (React, the router, TanStack Query, the shell, the kit) | 369.57 kB (115.28 kB) |
| Main stylesheet `index-*.css` | 63.87 kB (11.72 kB) |
| The chart chunk (uPlot and the Experiments shared code) | 66.12 kB (28.63 kB) |
| Largest page chunk, `ProjectPage-*.js` | 24.84 kB (7.20 kB) |
| `index.html` | 0.58 kB (0.37 kB) |
| Fonts, five IBM Plex WOFF2 files | 100.62 kB in total |
| `dist/` on disk | 1.5 MB |

The Ask Orama panel, and with it the Markdown renderer, loads the first time it is opened
(`React.lazy` in `components/Shell.tsx`), so the entry script carries neither. Before that it was
560.05 kB (174.57 kB).

To measure again: `cd frontend && npm ci && npm run build`, and read the size table Vite prints.
In a browser, open a page with the Network panel's cache disabled for the first load and enabled
for the second: the second loads only `index.html` and the calls.

## Slow capabilities, measured on the dev site

These measurements were taken against the server-rendered pages on 2026-10-02 (lake#446). The
pages are gone, but the time was in the workspace calls, which the app's pages make the same way,
so they still say which capabilities are slow.

### The dev site, 2026-10-02

Read-only GETs against `lake.oramadev.net` (`oramasearch/lake:20261002b`) as the test user, five
requests per page, signed in through a Clerk ticket ([browser-testing.md](../deployment/browser-testing.md)).
Every page made the workspace calls its budget says, in one round, so the time is in the calls
themselves:

| Page | Total ms | Workspace ms | Calls |
|---|---|---|---|
| Ask Orama, empty | 413–721 | 383–688 | 1 (`list_agents`) |
| Train → Agents | 406–689 | 382–663 | 1 (`list_agents`; the library is lake's) |
| Train → Agent (one agent) | 865–1,025 | 1,999–2,575 summed | 3 (`get_agent`, `agent_training_status`, `list_projects`) |
| Search Indexes (no indexes) | 107–288 | 81–455 summed | 2 (`list_indexes`, `list_datasets`) |
| Models (15 models) | 91–186 | 43–142 | 1 (`list_models`) |
| Experiments | 59–107 | 46–113 summed | 2 (`list_studies`, `list_runs`) |
| Data | 83–129 | 89–199 summed | 2–3 (`system.datasets`, `list_connections`, generators) |
| Pipeline | 128–290 | 97–420 summed | 2 (`list_runs`, `list_datasets`; the graph was cached) |
| Project | 104–295 | 65–366 summed | 2–4 |
| Runs | 31–140 | 8–115 | 1 (`list_runs`) |
| Projects | 44–68 | 13–40 | 1 (`list_projects`) |

The same transport answers `list_runs` in 8–34 ms and `list_projects` in 13–40 ms, so lake's own
part of a workspace call is small. The workspace has one agent, so `list_agents` costs 380–690 ms
whatever it returns, and the agent page's two agent-store reads cost far more together than either
alone. Lake's own work on a page with one call was 20–45 ms (Runs 23 ms, Models about 45 ms); a
lake-local registry read such as `server_info` or `workspace_status` took 12–19 ms end to end.

### Where the time goes, in Zetta

These are Zetta capabilities that are slow themselves, read from the reference checkout
(`modules/zetta-bridge/modules/zetta`, `a1d74b21`); lake cannot make them faster:

- **`list_agents`, `get_agent`, `agent_training_status`.** `zetta::agent::AgentStore::list()` and
  `get()` (`src/core/agent/agent_store.cpp`) call `ReplicationManager::snapshot()`, which copies the
  whole replicated catalogue JSON under the replication mutex on every call
  (`src/core/cluster/replication.cpp`), then scan every key for the agent prefix. Cost grows with
  everything replicated (projects, graphs, runs), not with the number of agents, and concurrent
  reads serialize on the mutex: the agent page's two reads took 2.0–2.6 s together. A fix is to
  read `shared_snapshot()` (no copy) and look up `agent_key(id)` directly; the same full copy is in
  `decision_policy.cpp`, `roi_readout.cpp` and `replicated_blob_store.cpp`.
- **`list_datasets` and `system.datasets`.** Both come from `Node::datasets()`, which scans every
  table's rows and directories to summarize it ([zetta#1195](https://github.com/oramasearch/zetta/issues/1195)).
  Lake's projected SQL only shrinks the answer. The Pipeline, Project and Solution dataset pickers
  and Usage still wait for it.
- **`list_models`** took 43–142 ms for 15 models, so the Models page's main read stays near
  100 ms until that read is faster.

The conversation case of the review (1.5–3.9 s) could not be reproduced with read-only requests.
Its only Zetta read was the same `list_agents`; while a turn runs, the agent's own workspace reads
contend with that copy for the replication mutex.

In the app: Train → Agents and Ask Orama's *Talk to* list (page and panel) call `list_agents`
after the page is on screen, and fill in when it answers; the Agent page waits for `get_agent` and
`agent_training_status`; Pipeline, Project and Solution wait for `list_datasets` for their pickers;
Usage's `metering_usage` measures the workspaces' dataset bytes on each read. Their panels show the Zetta times above until Zetta's agent store and dataset
listing are faster.

## How to verify and change

- Open a page with the Network panel and read each `/v1` call's `Server-Timing`: one
  `zetta_<capability>` entry per workspace call, and `store` entries for repository-store work.
- `curl -s -D - -o /dev/null -H 'Authorization: Bearer <token>' <origin>/v1/server_info` shows the
  header from the command line.
- After a deployment, walk the pages and record the slow calls by capability; `/metrics` has the
  per-route histogram.

A page that needs many calls should start them together, and a page should not wait for a slow
capability to show what it already has. Keep the bundle split by page: import a heavy library only
from the pages that use it.
