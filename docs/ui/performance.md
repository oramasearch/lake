# Page performance

**Status: implemented 2026-09-29.** Every HTML response carries a `Server-Timing` header, and
the page regression test enforces exact workspace call and sequential-round budgets for every GET
route that the live router identifies as an HTML page. A p95 below 500 ms over 20 cold-cache
renders remains only as a guard against pathological stalls. The live budget remains 100 ms and is
checked from access logs after deployment.

## Reading `Server-Timing`

The browser's Network panel shows three entries for a lake page:

- `total` is lake's server time from the first request middleware until the HTML response is
  ready, including workspace selection, authentication, and synchronous rendering.
- `workspace` is the accumulated duration of calls to the customer's workspace. Its description
  gives the number of calls, for example `3 calls`.
- `workspace_rounds` counts sequential outbound rounds. Concurrent reads started before any
  earlier read completes belong to the same round; a follow-up after completion starts another.

Workspace event-bus reads for apps, builds, and agent tasks use the same timing path as ordinary
workspace capabilities, including the app-detail page's event section.

An example header is:

```text
Server-Timing: total;dur=47.418, workspace;dur=30.731;desc="1 calls", workspace_rounds;desc="1 rounds"
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
and stream details overlap message browsing. Authorization and input validation still apply to
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
| `/w/:workspace_id/ask` | 1 | 1 |
| `/w/:workspace_id/design/discover` | 1 | 1 |
| `/w/:workspace_id/design/methodologies` | 1 | 1 |
| `/w/:workspace_id/design/methodology` | 2 | 1 |
| `/w/:workspace_id/design/opportunities` | 1 | 1 |
| `/w/:workspace_id/design/pipeline` | 3 | 1 |
| `/w/:workspace_id/design/project` | 9 | 2 |
| `/w/:workspace_id/design/projects` | 1 | 1 |
| `/w/:workspace_id/design/solution` | 3 | 1 |
| `/w/:workspace_id/design/solutions` | 1 | 1 |
| `/w/:workspace_id/ship/app` | 1 | 1 |
| `/w/:workspace_id/ship/index` | 1 | 1 |
| `/w/:workspace_id/ship/indexes` | 2 | 1 |
| `/w/:workspace_id/ship/models` | 1 | 1 |
| `/w/:workspace_id/train/agent` | 3 | 1 |
| `/w/:workspace_id/train/agents` | 1 | 1 |
| `/w/:workspace_id/train/data` | 3 | 1 |
| `/w/:workspace_id/train/dataset` | 2 | 1 |
| `/w/:workspace_id/train/experiment` | 2 | 1 |
| `/w/:workspace_id/train/experiment/run` | 1 | 1 |
| `/w/:workspace_id/train/experiments` | 2 | 1 |
| `/w/:workspace_id/train/experiments/compare` | 2 | 1 |
| `/w/:workspace_id/train/run` | 2 | 1 |
| `/w/:workspace_id/train/runs` | 1 | 1 |
| `/w/:workspace_id/train/stream` | 2 | 1 |
| `/w/:workspace_id/train/streams` | 1 | 1 |

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
Lake now requests just `database` and `name` with
`SELECT database, name FROM system.datasets`; counts, byte totals, format and samples are
shown on the dataset page, whose SQL sample uses `LIMIT 20`.
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
`list_runs` concurrently, so its budget is two calls in one round for the fixture's one workspace.

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
