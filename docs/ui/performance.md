# Page performance

**Status: implemented 2026-09-29.** Every HTML response carries a `Server-Timing` header, and
the page regression test enforces a p95 below 100 ms over 20 renders for every GET route that the
live router identifies as an HTML page.

## Reading `Server-Timing`

The browser's Network panel shows two entries for a lake page:

- `total` is lake's server time from route dispatch until the HTML response is ready.
- `workspace` is the accumulated duration of calls to the customer's workspace. Its description
  gives the number of calls, for example `3 calls`.

An example header is:

```text
Server-Timing: total;dur=27.418, workspace;dur=20.731;desc="1 calls"
```

Concurrent workspace calls contribute their individual durations to `workspace`, so that value
can exceed `total`. The count exposes accidental fan-out even when the calls overlap.

## Workspace page reads

List pages consume complete list records. Runs use the project and pipeline names returned by
`list_runs`; Models use the names, run id, metrics, publication state, and feature schema returned
by `list_models`; Experiments uses `list_studies` and `list_runs`. These pages do not look up every
project, run, or model after listing them. Independent sections use concurrent coroutine reads.

Zetta issue [#1184](https://github.com/oramasearch/zetta/issues/1184) tracks the denormalized names
and complete run/model/study list records. Zetta issue
[#1185](https://github.com/oramasearch/zetta/issues/1185) tracks project summaries with graph id,
name, and approval state for the agent page. Until #1185 is available, a legacy string-only
`list_projects` response leaves the approved-pipeline selector empty instead of issuing one
`get_project` request per project.

## Regression fixture

`PagePerformance.EveryRegisteredGetPageStaysUnderTheServerBudget` starts the production HTTP
router and obtains its GET patterns from `ApiServer::get_route_patterns()`. It probes every
pattern, identifies HTML responses by content type and `Server-Timing`, and measures each page 20
times. Adding a GET page automatically adds it to the measured set.

The fake workspace waits 20 ms per call and returns 50 projects, 200 runs, 50 models, 20 studies,
50 topics, and the control store holds 20 apps. The p95 for every discovered page must be below
100 ms. This latency makes a fifth serial workspace round trip fail the budget while allowing
independent reads to overlap.

The test prints one stable evidence row per page:

```text
PAGE_PERF /ship/models p95_ms=26.4
```

The values are synthetic regression evidence, not a substitute for the post-deploy access-log
walk. Production is checked after deployment by opening every page once and confirming every
server duration remains below 100 ms.
