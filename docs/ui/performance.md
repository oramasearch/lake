# Page performance

**Status: implemented 2026-09-29.** Every HTML response carries a `Server-Timing` header, and
the page regression test enforces exact workspace call and sequential-round budgets for every GET
route that the live router identifies as an HTML page. A p95 below 500 ms over 20 cold-cache
renders remains only as a guard against pathological stalls. The live budget remains 100 ms and is
checked from access logs after deployment.

**Updated 2026-10-02 (lake#446).** `Server-Timing` names every workspace call by its Zetta
capability. Ask Orama, Train → Agents and Search Indexes no longer wait for the Zetta reads that
made them slow on the dev site: those panels load after the page from three deferred panel routes.
The regression test now also holds the exact Zetta capabilities of those pages and the panels, and
refuses any page that waits for a capability measured slow on the dev site, except the pages named
in [Live budget follow-up](#live-budget-follow-up-lake446).

## Reading `Server-Timing`

The browser's Network panel shows these entries for a lake page:

- `total` is lake's server time from the first request middleware until the HTML response is
  ready, including workspace selection, authentication, and synchronous rendering.
- `workspace` is the accumulated duration of calls to the customer's workspace. Its description
  gives the number of calls, for example `3 calls`.
- `workspace_rounds` counts sequential outbound rounds. Concurrent reads started before any
  earlier read completes belong to the same round; a follow-up after completion starts another.

Workspace event-bus reads for apps, builds, and agent tasks use the same timing path as ordinary
workspace capabilities, including the app-detail page's event section.

- `zetta_<capability>` is one workspace call, named by the Zetta capability it called
  (`zetta_list_models`, `zetta_sql` for a `workspace_sql` read), with its own duration. There is one
  entry per call, in completion order, so a slow page names the engine read it waited on.

An example header is:

```text
Server-Timing: total;dur=47.418, workspace;dur=30.731;desc="1 calls", workspace_rounds;desc="1 rounds", zetta_list_models;dur=30.731
```

Concurrent workspace calls contribute their individual durations to `workspace`, so that value
can exceed `total`. The count exposes accidental fan-out even when the calls overlap.

## Workspace page reads

List pages consume complete list records. Runs use the project and pipeline names returned by
`list_runs`; Models use the names, run id, metrics, publication state, and feature schema returned
by `list_models`; Experiments uses `list_studies` and `list_runs`. These pages do not look up every
project, run, or model after listing them. Independent sections use concurrent coroutine reads.
Agent details, training status, and approved-pipeline summaries share one concurrent batch.
Dataset metadata and its sample query also run concurrently through their existing authorized
capabilities; the sample remains bounded to 20 displayed rows.
Project-to-graph reads run alongside dataset and methodology catalogues, without waiting for
unrelated catalogue responses before starting graph reads. Study details overlap analysis reads,
and stream details overlap message browsing. A stream's creator is a record in the same workspace
(lake#379), read by `get_record` alongside `get_topic`, so the stream page went from two calls to
three and stays at one round; the PostgreSQL ownership query it replaces is gone. Authorization and input validation still apply to
each capability independently.

Solution and solution-backed Project pages also start one bounded `list_runs` registry read in
those batches. Its retained `get_run` details start together, so five rows add calls without adding
one sequential round per row. Project reuses the completed run rows while reading policy,
experiment, ledger, monitor and ROI in its existing concurrent state batch.

Zetta issue [#1184](https://github.com/oramasearch/zetta/issues/1184) tracks the denormalized names
and complete run/model/study list records. Zetta issue
[#1185](https://github.com/oramasearch/zetta/issues/1185) tracks project summaries with graph id,
name, and approval state for the agent page. Until #1185 is available, a legacy string-only
`list_projects` response leaves the approved-pipeline selector empty instead of issuing one
`get_project` request per project.

## Regression fixture

Workspace URL rewriting scans links once, tracks script state while moving forward, and appends
unchanged spans plus the workspace prefix to a new buffer. Form fields use another forward pass.
Neither operation repeatedly searches the document prefix or inserts into the middle of the
document, so cost grows linearly with rendered HTML. The shell and response boundary may both
apply this idempotent rewrite without changing an explicit workspace URL or duplicating a field.

`WebUi.WorkspaceRewrite*` covers script navigation, URL attributes, query strings, fragments,
global and external links, existing workspace selections, incomplete forms, and 2,000-row pages.
Keep these tests and `WebUi.WorkspacePathOwnsNavigationAndEveryFormCarriesTheSelection` when
changing the rewrite or moving URL generation into individual renderers.

`PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget` starts the production HTTP
router and obtains its GET patterns from `ApiServer::get_route_patterns()`. It probes every
pattern, identifies HTML responses by content type and `Server-Timing`, and measures each page 20
times. Adding a GET page automatically adds it to the measured set.

Canonical `/w/:workspace_id/...` page patterns are expanded with a real fixture workspace ID.
Every such page and `/workspaces` must return successful HTML containing the workspace selector;
a redirect or error response fails coverage. The Zetta JSON proxy patterns are not page routes.
Apps belong to that workspace, and the app detail route selects one of the 20 real fixture apps.
Image-mode app detail rendering accepts its absent source repository (`source: null`).
Models and Runs must each make one workspace call; Experiments must make two, concurrently.
Every HTML response's reported call count must also match the fake workspace's observed count.

Before discovery and every measured render, the fixture invalidates the five-second workspace
definition cache. Each response must exactly match the following cold call/round budget. Every
route not listed here has an implicit `0` calls and `0` rounds budget, so a new page cannot start
workspace work without an explicit review of this table and the test map.

| Route | Calls | Rounds |
|---|---:|---:|
| `/w/:workspace_id/ask` | 0 | 0 |
| `/w/:workspace_id/design/discover` | 2 | 1 |
| `/w/:workspace_id/design/methodologies` | 1 | 1 |
| `/w/:workspace_id/design/methodology` | 2 | 1 |
| `/w/:workspace_id/design/opportunities` | 2 | 1 |
| `/w/:workspace_id/design/pipeline` | 3 | 1 |
| `/w/:workspace_id/design/project` | 9 | 2 |
| `/w/:workspace_id/design/projects` | 1 | 1 |
| `/w/:workspace_id/design/solution` | 3 | 1 |
| `/w/:workspace_id/design/solutions` | 1 | 1 |
| `/w/:workspace_id/ship/app` | 1 | 1 |
| `/w/:workspace_id/ship/index` | 1 | 1 |
| `/w/:workspace_id/ship/indexes` | 1 | 1 |
| `/w/:workspace_id/ship/models` | 2 | 1 |
| `/w/:workspace_id/train/agent` | 3 | 1 |
| `/w/:workspace_id/train/agents` | 0 | 0 |
| `/w/:workspace_id/train/data` | 3 | 1 |
| `/w/:workspace_id/train/dataset` | 3 | 1 |
| `/w/:workspace_id/train/experiment` | 2 | 1 |
| `/w/:workspace_id/train/experiment/run` | 1 | 1 |
| `/w/:workspace_id/train/experiments` | 2 | 1 |
| `/w/:workspace_id/train/experiments/compare` | 2 | 1 |
| `/w/:workspace_id/train/run` | 3 | 1 |
| `/w/:workspace_id/train/runs` | 2 | 1 |
| `/w/:workspace_id/train/stream` | 3 | 1 |
| `/w/:workspace_id/train/streams` | 1 | 1 |

Train → Runs, a run page and Ship → Models each add a concurrent `list_studies` read, so a study's
trial runs and models are named by their study (lake#449). A study page with trials reads up to
eight trials' `get_run_metrics` in a second round, after the study names them; the fixture's study
has none, so its budget stays at one round.

Design → Opportunities (and `/design/discover`, the same page) reads its saved Opportunities from
the workspace's records (`list_records`, lake#376) concurrently with `list_methodologies`; both
capabilities are fixed for it.

Five pages also have their exact Zetta capabilities fixed, one entry per call: Ask Orama and
Agents none, Search Indexes `list_indexes`, Models `list_models` and `list_studies` (lake#449),
Experiments `list_runs` and `list_studies`. Every response's `zetta_<capability>` entries must equal the capabilities the fake
workspace observed for it.

`/application/events` is not a workspace route and the fixture has no application, so it is held at
the implicit `0`. With an application that has events it reads its workspace twice, in two rounds:
the newest events, then their delivery attempts back to the oldest of them (lake#356).

The deferred panels are JSON routes, not pages, and are held to their own budgets:

| Panel | Calls | Rounds | Capabilities | Filled into |
|---|---:|---:|---|---|
| `/w/:workspace_id/ask/agents` | 1 | 1 | `list_agents` | Ask Orama's *Talk to* selector |
| `/w/:workspace_id/train/agents/list` | 1 | 1 | `list_agents` | Train → Agents' *Your Agents* |
| `/w/:workspace_id/ship/indexes/datasets` | 1 | 1 | `sql` (projected `system.datasets`) | Search Indexes' *New Index* form |

`web_ui_deferred_panels` lists them; the test requires each to answer 200 JSON without an error for
the signed-in user. Without JavaScript, Agents and Search Indexes link to `?inline=1`, which renders
the panel in place and waits for it; Ask Orama's selector keeps Ask Orama and the library agents.

No page may wait for a Zetta capability measured slow on the dev site: `list_agents`, `get_agent`,
`agent_training_status` (the agent store copies Zetta's whole replicated catalogue) and the full
`list_datasets` (it scans every table). The exceptions, all over the live budget until Zetta is
fixed, are the agent page (`get_agent`, `agent_training_status`), Pipeline, Project and Solution
(`list_datasets` for their dataset pickers) and Usage (`list_datasets`, for byte totals).

An intentional page-read change updates this table and the test map together. A higher round count
also requires showing why the reads cannot start concurrently. An extra call or round fails on the
first affected response, independently of elapsed time.

The fixture uses an ordinary signed-in user and the production PostgreSQL store so that Ask and
account pages render with a user identity and session lookup is included in the measured time.
For non-workspace routes that do not return HTML to that user, discovery also tries the operator
and signed-out profiles. This includes operator-only pages and sign-in forms that would otherwise
redirect. Each measured page retains the profile that produced its HTML response.
Set `LAKE_TEST_DB_URI` for this test; the shared test helper creates and removes a fresh database.
Without it the performance test is reported as skipped, not passed.

The fake workspace waits 30 ms per call and returns 50 projects, 200 runs, 50 models, 20 studies,
50 topics, and the control store holds 20 apps. The p95 for every discovered page must be below
500 ms. This loose limit detects a stuck or pathologically slow route without turning host
scheduling into the primary assertion. It is not evidence that the live 100 ms requirement passes.

The test prints one stable evidence row per page:

```text
PAGE_PERF /w/:workspace_id/ship/models p95_ms=46.400 calls=1 rounds=1
```

Each `PAGE_TIMING` row preserves the actual `Server-Timing` header for that page's p95 sample,
so a failure distinguishes workspace latency from other server work.

The values are synthetic regression evidence, not a substitute for the post-deploy access-log
walk. Production is checked after deployment by opening every page once and confirming every
server duration remains below 100 ms.

## Workspace-route verification

The 2026-09-29 native macOS verification after the workspace-route merge measured 70 HTML page
patterns in each build, with 20 renders per pattern. The maximum p95 was 98.057 ms in the static
build and 94.610 ms in the shared build. Both maxima were the project detail page.

| Page | Earlier isolated p95 (ms) | Static p95 (ms) | Shared p95 (ms) |
|---|---:|---:|---:|
| Models | 74.878 | 53.574 | 61.899 |
| Experiments | 65.315 | 61.756 | 55.618 |
| Runs | 194.660 | 57.433 | 65.009 |
| Agent | 107.640 | 49.517 | 69.108 |
| Dataset | 103.778 | 50.910 | 57.190 |
| Workspaces | 13.521 | 11.265 | 14.929 |

The earlier isolated measurements already included workspace selection and PostgreSQL session
lookup but preceded the linear HTML rewrite and remaining read-concurrency changes. The complete
per-page figures and native suite output are recorded in [PR #387](https://github.com/oramasearch/lake/pull/387).
These figures do not claim a deployment or live-site verification.

## Headroom follow-up

The follow-up starts at deployed `9268f4f` (`lake:20260929b`). The live requirement remains
below 100 ms; the regression budget is now below 80 ms at 30 ms per workspace call.

The reported live before values were Data 93, 99, 118, 120, 120 ms with 19 datasets;
Methodologies 103, 66, 92, 84, 73 ms; and Metering 256 ms on its first load followed by about
36 ms. Read-only access-log inspection confirmed those slow requests. Additional measurements
used the documented dev-cluster connection procedure and operator-authenticated reads through
a port-forward; they did not modify deployment configuration or workspace data.

Twenty individual `workspace_query` reads per catalogue gave these capability-envelope p95s,
including registry overhead: datasets 30.439 ms, connections 25.965 ms, generators 26.770 ms,
methodologies 21.697 ms, solutions 27.220 ms. The fake delay therefore increased from 20 to
30 ms. During that measurement the selected live workspace returned zero datasets, so it does
not reproduce the earlier 19-dataset population. Populated live after figures require deployment.

### Data and local work

Data already made one `list_datasets` call, not per-dataset Lake reads. The upstream
`Node::datasets()` implementation scans stored rows for columns, table directories for JSONL
byte totals and partition placement. Its list contract has no projection argument.
Lake now requests just the database and the dataset name with
`SELECT database, dataset FROM system.datasets` (the view names the table column `dataset`;
asking for `name` left every Data row blank until lake#447); counts, byte totals, format and
samples are shown on the dataset page, whose SQL sample uses `LIMIT 20`. The dataset page's
third call, `analyze_dataset` over 256 sampled rows, runs in the same round and gives the column
types (lake#447).
Zetta still materializes summaries before SQL projection.
[zetta#1195](https://github.com/oramasearch/zetta/issues/1195) requests removal of that upstream
scan; the read-only reference checkout was not changed.

Independent Lake services/storage reads run alongside Data's workspace reads, and agent launch
and schedule reads run alongside agent details. Data, Runs and Agents overlap their local
workspace-status reads with the remote lists; Agents also overlaps its library read. Project
catalogues start before cached definition processing, so synchronous authorization and audit
work happens during the remote wait. Every read still uses the registry.
GET middleware, handlers and navigation reuse
one resolved authentication context and authorized customer-list result within the request.
No authentication or customer-list result survives into the next request; explicit workspace
selection, capability authorization and audit behavior remain intact.

### Registry cache correctness

Methodologies no longer reads the unused approximately 64 KB solutions catalogue. Detail pages
retain the catalogue. `WorkspaceService` caches only successful `list_methodologies`,
`list_solutions`, and `list_dataset_generators` results with empty input. Renderer-only
`get_project` and `get_graph` reads may also opt in; editing operations always read current
definitions. Mutable dataset, connection and run lists, execution state and usage are not cached.

The Project page still measured 95.115 ms p95 with two sequential workspace rounds after
request-local reuse and independent-read batching. Its definition cache addresses that remaining
miss; the dataset catalogue alongside it remains a fresh read on every render.

The cache holds at most 128 entries for five seconds. Keys include customer, workspace, service,
generation, endpoint, credential, capability and input. Ownership, readiness and the mounted
credential are rechecked before every hit. Successful workspace actions call
`invalidate_registry(customer_id, workspace_id)`, which also serves explicit workspace-change
notifications. An epoch prevents an in-flight pre-invalidation read from repopulating the cache.
Direct engine changes or a deployment retaining its connection identity become visible after
expiry; connection changes select a new key immediately. Cache hits do not count as outbound
workspace calls. Entries and their credentials remain only in process memory.

`Workspaces.Registry*` covers expiry, explicit and action invalidation, all three catalogues,
workspace isolation, token/endpoint changes, unready/foreign workspaces, failed reads and stale
in-flight replies. `Workspaces.PipelineDefinitionCache*` checks input isolation, bypass for edits
and refresh after saving. Keep these tests whenever changing the allow-list, TTL or key.

### Metering and populated coverage

Startup opens existing repository result indexes before accepting traffic. Storage-byte reads
inspect pack metadata without creating an empty store; absent repository storage has no results.
Usage itself is never cached. The performance fixture creates four repositories containing
128 files, closes them, starts a new server, and makes Metering its first request. That request
must report nonzero stored bytes. Its elapsed time remains in the evidence output, while the
router-derived Metering samples carry the same 500 ms p95 guard as every other page.
Since lake#444 the Usage page (`/metering`) also reads each workspace's `list_datasets` and
`list_runs` concurrently. Since lake#348 and lake#349 it reads, in the same round, one
`list_records` each of the workspace's repository transfer and build records, so its budget is four
calls in one round for the fixture's one workspace. A sync request makes no workspace call: its
transfer is written by the owner loop ([metering](../api/metering.md#transfer-off-the-request-path)).

The fixture now supplies 50 datasets, 21 methodologies, 20 solutions and three generators, not
empty catalogues. It measures 71 HTML patterns, including populated Metering, all canonical
workspace pages and Workspaces. An additional 20-render Data case changes the population to
19, proves the next page sees the change, and rejects per-item metadata/sample reads.

The initial stricter run failed 15 routes: Pipeline, Project, Projects, Solution, App, Agent,
Agents, Data, Experiment, Experiment Run, Experiments, Compare Runs, Run, Runs and Streams.
Its largest p95 was 148.306 ms (Project); Data was 93.551 ms. Host load was
`{ 6.35 5.70 5.82 }`, so those failures are not classified as host contention.
Final per-route native figures and proof output are recorded with the follow-up PR.

### Native headroom verification

The final macOS Release static/shared route proofs each measured all 71 patterns at 30 ms
fake latency, with 20 renders per pattern. Every p95 was below 80 ms. The largest static p95
was 69.242 ms (Project); the largest shared p95 was 65.373 ms (App). No measured page exceeded
one outbound round during those samples; the assertion permits at most two, including dependent
definition reads on a cache miss.

Both complete native suites report `100% tests passed, 0 tests failed out of 441` against
separate fresh local PostgreSQL databases. Each reports 14 skips and one disabled benchmark;
the performance test ran in both. Static and shared binaries also passed version, health,
readiness and HTML `Server-Timing` smoke checks. Linux proof is deferred because the provided
Linux-lane helper is absent; no container or image build ran on this host.

| Case | Static ms | Shared ms |
|---|---:|---:|
| Data, 50 datasets, p95 | 49.170 | 45.369 |
| Data, 19 datasets, p95 | 48.528 | 46.258 |
| Methodologies, p95 | 5.819 | 5.552 |
| Project, p95 | 69.242 | 59.038 |
| Metering, populated first request | 15.734 | 12.688 |
| Metering, subsequent p95 | 5.710 | 7.673 |

These are native regression measurements, not live after-deploy figures. The live access-log
walk and the removal of Zetta's dataset-summary scan remain separate verification/work items.

## Deterministic budget follow-up

Issue [lake#390](https://github.com/oramasearch/lake/issues/390) replaced the 80 ms synthetic
regression threshold after unrelated routes repeatedly crossed it on an otherwise idle host. The
test now makes the controlled quantity primary: the exact number of workspace calls and the exact
number of sequential rounds in each response's `Server-Timing` header. Cache invalidation before
every render makes those values independent of route order and cache age. The 500 ms p95 check is
only a loose guard; the live 100 ms access-log check did not change.

Twenty consecutive static runs passed all 71 routes with 20 renders per route. The one-minute load
at the start of those runs reached 18.86. The largest p95 was 344.824 ms during the first and
busiest run; the remaining maxima ranged from 84.832 to 188.488 ms. Every call and round count
matched its route budget.

After the subsequent upstream rebase, another twenty consecutive static runs passed the same
71-route matrix. Their starting one-minute loads ranged from 2.18 to 3.73, and their maximum route
p95s ranged from 101.344 to 136.129 ms. This final-base repeat also matched every call and round
budget. The final static and shared native suites each reported no failures across 451 tests
against separate fresh PostgreSQL databases; the performance regression ran in both.

For the mutation check, the Ask handler temporarily repeated its one workspace read after the
first completed. The test failed during route discovery with:

```text
Expected equality of these values:
  workspace_call_count(response)
    Which is: 2
  budget.calls
    Which is: 1
/w/:workspace_id/ask
```

The duplicate read was then removed. Reproduce the passing check with a PostgreSQL fixture by
running `lake-tests` with the filter
`PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget`; repeat that command twenty
times for the acceptance proof. A deliberate mutation should use break-on-first-failure so the
route budget failure remains the relevant output.

## Live budget follow-up (lake#446)

The UX review of 2026-10-01 measured, as `Server-Timing` total on the second request: Ask Orama
417 ms empty and 1.5–3.9 s with a conversation, Train → Agents 684 ms, Search Indexes 286 ms,
Models 143 ms, Experiments 108 ms; an Ask Orama turn that delegated to ExperimentsAgent took 180 s,
and the first `predict` 5.6 s.

### Before: the dev site, 2026-10-02

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
- **`list_models`** took 43–142 ms for 15 models. With lake's 20–45 ms Models stays near the
  100 ms budget until that read is faster.

The conversation case of the review (1.5–3.9 s) could not be reproduced with read-only requests.
Its only Zetta read was the same `list_agents`; while a turn runs, the agent's own workspace reads
contend with that copy for the replication mutex. The page no longer makes the read.

### What changed in lake

- **Ask Orama renders with no workspace call.** The *Talk to* selector starts with Ask Orama and the
  library's chat agents (lake's own `list_agent_library`), keeps the conversation's agent selected,
  and adds the space's own agents from `/ask/agents` after the page loads.
- **Train → Agents renders with no workspace call.** *Your Agents* fills in from
  `/train/agents/list`, which lake renders with the same code as before (`web_ui_your_agents`).
- **Search Indexes makes one call** (`list_indexes`). The *New Index* form loads from
  `/ship/indexes/datasets`, which reads the projected `system.datasets` list.
- **A long Ask Orama turn** shows which agent is working, for how long, its latest message and
  retries, and the agent it brought in, through lake#451's report on the asking turn
  ([ask-orama.md](../api/ask-orama.md)). That page read is lake's own `get_agent_task`, not Zetta.
- **`Server-Timing` names each Zetta call** (`zetta_<capability>;dur=`), so the live walk after a
  deployment reads the slow engine call directly from the page and from each panel.

Models and Experiments keep their single round: their lists are the page. Since lake#449 Models
also reads `list_studies` alongside `list_models`, in the same round. Their time is the Zetta
read plus lake's 20–45 ms.

### After

Native macOS Release, `PagePerformance.EveryRegisteredGetPageKeepsItsWorkspaceCallBudget` at 30 ms
per workspace call, 20 renders per page (p95, ms). The host was shared with other builds:

| Page | Before: calls, rounds | Before p95 | After: calls, rounds | After p95 |
|---|---|---:|---|---:|
| Ask Orama | 1, 1 | 56.8 | 0, 0 | 3.0 |
| Train → Agents | 1, 1 | 52.9 | 0, 0 | 23.8 |
| Search Indexes | 2, 1 | 60.8 | 1, 1 | 77.4 |
| Models | 1, 1 | 58.0 | 1, 1 | 84.7 |
| Experiments | 2, 1 | 57.5 | 2, 1 | 84.6 |
| Runs (unchanged, for scale) | 1, 1 | 56.3 | 1, 1 | 76.1 |

The after run was on a busier host (Runs, which did not change, went from 56 to 76 ms); the
call and round counts are the controlled result. Ask Orama and Agents no longer wait for any
workspace call. Later full-suite runs on the same host, at one-minute load 27–102 from other
builds, failed only the 500 ms wall guard on unchanged routes; every call, round and capability
budget held.

These are regression measurements, not live figures. The live walk after the next deployment
records the per-page totals and the `zetta_<capability>` entries; until Zetta's agent store and
dataset listing are fixed, Train → Agent, Pipeline, Project, Solution, Usage and the three deferred
panels will report the Zetta times above, and Models stays close to the budget.
