# Page performance

**Status: implemented 2026-09-29.** Every HTML response carries a `Server-Timing` header, and
the page regression test enforces a p95 below 100 ms over 20 renders for every GET route that the
live router identifies as an HTML page.

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

`PagePerformance.EveryRegisteredGetPageStaysUnderTheServerBudget` starts the production HTTP
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

The fixture uses an ordinary signed-in user and the production PostgreSQL store so that Ask and
account pages render with a user identity and session lookup is included in the measured time.
For non-workspace routes that do not return HTML to that user, discovery also tries the operator
and signed-out profiles. This includes operator-only pages and sign-in forms that would otherwise
redirect. Each measured page retains the profile that produced its HTML response.
Set `LAKE_TEST_DB_URI` for this test; the shared test helper creates and removes a fresh database.
Without it the performance test is reported as skipped, not passed.

The fake workspace waits 20 ms per call and returns 50 projects, 200 runs, 50 models, 20 studies,
50 topics, and the control store holds 20 apps. The p95 for every discovered page must be below
100 ms. This latency makes a fifth serial workspace round trip fail the budget while allowing
independent reads to overlap.

The test prints one stable evidence row per page:

```text
PAGE_PERF /w/:workspace_id/ship/models p95_ms=26.400
```

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
