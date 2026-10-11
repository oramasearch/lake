# Train → Experiments

Experiments follows Runs in Train. The pages are in `frontend/src/pages/experiments/`:

| Route | File | What |
|---|---|---|
| `/w/<id>/train/experiments` | `ExperimentsPage.tsx` | Run an Experiment (ExperimentsAgent), the filter, Start a Study, the studies, tracked runs outside studies |
| `/w/<id>/train/experiment?study=` | `StudyPage.tsx` | one study: status, Stop, optimization history, parallel coordinates, parameter importance, the leaderboard, the latest eight trials' curves |
| `/w/<id>/train/experiment/run?run=` | `RunPage.tsx` | one run's curves, parameters and artifacts |
| `/w/<id>/train/experiments/compare?runs=run-a,run-b` | `ComparePage.tsx` | two to eight runs on shared charts with their parameter differences |

All four mark Experiments current in the rail. Runs and Run reports link to the metric charts.
`common.tsx` holds what they share (the heading, the trail, the confirmed actions, the live loop,
the metric charts and their toolbar); `transforms.ts` the chart arithmetic; `Chart.tsx` one uPlot
chart.

The list reads `list_experiments` (studies and tracked runs, with every summary field the page
shows, so it never calls `get_run_metrics` once per run). It shows each study's state, objective,
best complete-trial value, trial counts and trial/cost budgets. Objectives are labelled by Zetta's
canonical held-out metric names (`held_out_auc`, `held_out_log_loss`, `held_out_accuracy`,
`held_out_rmse`, `held_out_mae`); a study recorded under an older spelling such as `validation.auc`
is shown under its canonical name. The start form defaults the objective to `held_out_auc` and
offers those names (lake#463). An error names the field at fault when Zetta does, for example
`/objective: required; …`. Project and state filters are in the URL. Starting a study shows its
parameters and budgets in a confirmation dialog before `start_study` is called with `confirm: true`.
Study and trial stop buttons are shown to writers; Promote to space administrators
(`my_permissions` on the space: `write`, `admin`). No page shows a credential or contacts Zetta
directly.

## Layout (lake#561)

- **The list** opens with figures (Studies, Running, Trials, Tracked runs). Below them, a search
  over studies and runs, the state as filter chips (kept in `state=`) and the project filter
  (kept in `project=`). Studies are rich rows: the name, then the project and objective, the state
  chip, the best value with its direction, trials against the budget with complete/pruned/failed
  counts, cost against the cap, and Stop. Tracked runs have a compare checkbox and
  **Compare selected runs** in the card header.
- **Ask ExperimentsAgent** and **Start a study** open drawers from the header. The study form is
  unchanged and still confirms its parameters in a dialog.
- **Study** has the name and state in its header, figures (best objective, trials, running, cost),
  and a Split. The charts and the leaderboard (state chips as filters, Compare in its toolbar) are
  in the main column. Status, budgets with meters, and trials by state are in the side column.
  The latest trial curves come below.
- **Run** and **Compare** put the charts' toolbar in a card and each metric group in a card.
  Compare adds figures and the runs compared, as links. On every page except the list, the header
  holds All experiments and the **Live updates** switch with its status.
- **Compare with fewer than two runs** (or more than eight) shows "Choose Two or More Runs to
  Compare" with a way back to the tracked runs, and calls nothing: `compare_runs` takes two to
  eight distinct runs, and the page used to send the empty list and show lake's 400
  (`chosenRuns`, `canCompare` in `transforms.ts`, lake#576). A REST client can also call
  `GET /v1/compare_runs?customer_id=<space>&workspace_id=<workspace>&runs=<r1>&runs=<r2>` ([arrays in a query string](../api/README.md#arrays-in-a-query-string)).

## Run an Experiment

The list begins with a **Run an Experiment** box. A person describes a training or tuning goal;
the form starts an `agent_turn` for `agent-experiments` with kind `experiment`, puts the task in
the URL (`?task=`), and follows it with `get_agent_task` every two seconds. A completed proposal
shows the agent's explanation and its complete `study` JSON in an editable text area.

If the completed turn asks a question or returns no proposal, its answer is followed by a
**Reply** box, which starts another `experiment` turn in the same conversation, so ExperimentsAgent
can use the answer without the person restating the goal.

**Confirm and start study** reads the task again and checks, in the browser, that it is still a
succeeded ExperimentsAgent `experiment` proposal and that it is the proposal that was shown (its
JSON with sorted keys); then it parses the edited study and starts a second `agent_turn` carrying it
as `context.confirmed_proposal`. Malformed JSON or a changed proposal starts nothing. The first task
has no study action. Only a turn carrying a confirmed proposal gets `create_study` and
`start_study`; the agent then reaches the ordinary `start_study` policy, validation, confirmation
and audit path through its workspace adapter.

**What changed (lake#561).** The server's form checked the proposal digest against the stored task
for the same person and space before starting the turn. That check now runs in the page against a
fresh read; `agent_turn` grants the study tools to a turn whose `confirmed_proposal` has a study,
as it does for a REST or MCP caller.

ExperimentsAgent must be deployed (the Agent Library says so). Otherwise the box stays visible,
explains that it is unavailable, and disables submission. Ask Orama and the Project Agent can
delegate the same goal through `agent_turn`; there is no page-only experiment operation.

## Studies and Runs

A study shows objective history and its best-so-far line, normalized parallel coordinates,
parameter importance, trial states and a sortable leaderboard. The column picker includes trial
parameters and recorded final metrics. Confidence bounds come from the engine analysis. Each trial
links to its Run charts; two to eight selected trials can be compared. The latest eight trial
curves are drawn on the study page; a trial whose curves cannot be read is left out rather than
failing the page. Each of optimization history, parallel coordinates and parameter importance is a
chart over a table of its exact values. A promoted trial and the study's promoted model are marked
**promoted**. Leaderboard metric values are loaded for every trial with at most four concurrent
`get_run_metrics` requests.

Single-run and comparison pages draw every metric grouped by its prefix. Runs keep consistent
colours, with dashed lines supplementing the palette. The X axis can be step, seconds since each
run's first recorded point, or wall time. Smoothing is an EMA with displayed weight 0–99%:
`smoothed = weight * previous + (1 - weight) * current`; its first value is the first raw point.
Missing run/step pairs remain absent. Log scale omits nonpositive plotted values while keeping them
in the data table. Drag selects an X region, Shift-drag pans, and keyboard-accessible buttons zoom,
pan and reset across every metric chart. The hover crosshair and legend show values.
User-selected X bounds survive live refreshes; changing the axis resets them.

The parameter table marks differing values and distinguishes missing values from null. Artifacts
include confusion matrices, ROC/PR, calibration, feature importance and sample prediction tables.
Every chart has its exact data in a table (**Chart Data** under each metric). User-controlled
strings are rendered as text. A differential-privacy run says *curves suppressed for differential
privacy* and is excluded from curve drawing and live metric requests.

## Controls and Capabilities

| Control or data region | Capability |
|---|---|
| Run an Experiment goal, task state, editable proposal and confirmation | `agent_turn`, `get_agent_task` |
| Project and state filters and the list | `list_experiments` |
| Start form and confirmation | `start_study` |
| Study status and trial links | `get_experiment_study` |
| Trial states | `list_experiment_trials` |
| History, parallel coordinates, importance, sorting and columns | `get_experiment_analysis` |
| Single-run metrics, artifacts and presentation controls | `get_run_metrics` |
| Compare selection, overlays and parameter differences | `compare_runs` |
| Live toggle and polling | `tail_experiment_events` |
| Stop study or trial | `stop_study`, `stop_experiment_trial` |
| Promote and its confirmation | `promote_trial` |
| Which buttons show | `my_permissions` |

Presentation controls transform already-authorized capability results in the browser; they make no
private API calls. Stop and Promote ask for confirmation in words, with the exact input shown, and
send `confirm: true`; Escape or Cancel sends nothing. The same confirmation field and permission
checks apply to REST and MCP. The [API contract](../api/experiments.md) lists all schemas and
examples, including the naming distinction from solution holdouts.

## Live Updates

While **Live** is on and the tab is visible, each page long-polls `tail_experiment_events` for its
study and runs (`study-events.<study>`, `run-metrics.<run>`; at most 100 events, 10-second waits),
refreshes its durable state on any event and once more per cycle, at most every 10 seconds and at
least 1 second apart. The periodic refresh also repairs gaps when an ephemeral topic is absent.
At most eight run topics and one study topic are followed. A hidden tab or Live off pauses; a
failure shows *Reconnecting* and keeps trying.

## Charts

uPlot comes from npm (`uplot` in `frontend/package.json`) and is bundled into the app's hashed
`/ui/` files with its stylesheet; there is no chart CDN and no separately served chart asset.
`Chart.tsx` draws one chart in lake's colours, read from the design tokens, and redraws when the
theme changes. `styles/experiments.css` carries the pages' styles, scoped to `.experiments`, in
lake tokens only. The confirmation dialog is the kit's `Modal`.

Before lake#561 uPlot was vendored under `assets/experiments/` and served with a chart controller
(`experiments.js`) from content-hashed `/assets/experiments/` URLs; the pages drew their tables on
the server.

## How to Verify and Change

The chart arithmetic in `transforms.ts` has no unit test yet: `frontend/tests/` covers clicks,
the Design rules and formatting, and the CTest `lake-experiment-charts` still tests the old
`assets/experiments/experiments.js`. In a workspace with a trainer: start a study, watch trials and metric points arrive with Live on,
compare two runs, change the axis, smoothing and log scale, zoom and pan, open **Chart Data**,
cancel a promotion and then confirm it, and check both themes and a narrow width. The capabilities
are tested in `tests/test_experiments.cpp` and by `SchemaConformance.*`
([../api/experiments.md](../api/experiments.md)).

Keep chart arithmetic in `transforms.ts`, rendering in `Chart.tsx`, and every read and
write in a registered capability. Updating uPlot is a `package.json` and `package-lock.json`
change; check the charts in both themes after it.

Deployment acceptance must separately start a real study on the live site, observe real trial
curves, compare two real runs with smoothing and log scale, and exercise promotion confirmation.

Study deletion is deferred to [Zetta #1158](https://github.com/oramasearch/zetta/issues/1158),
as agreed for lake#303. The engine contract has no deletion operation, so the pages have no delete
control, not even a disabled one, and lake has no local-only hiding. Add the registered REST/MCP
operation and its UI control together once the engine contract exists.

## Native Verification Record — 2026-09-28

Historical: recorded against the server-rendered pages.

After rebasing onto `origin/main` at `52f0b30`, submodules were updated and both build paths were
recreated and configured from scratch. The previous build trees were retained under `.cache`,
outside the active build paths; no compiled objects were reused. The authorized Zetta and Liquid
source paths remained unchanged. Builds used `cmake --build build -j 6` and
`cmake --build build-shared -j 6`, with `BUILD_SHARED_LIBS=OFF` and `ON` respectively.

The full suites ran sequentially with fresh local connection databases:

```sh
LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/lake_issue303_static_20260928_final \
  ctest --test-dir build --output-on-failure
LAKE_TEST_DB_URI=postgresql://stevesperandeo@127.0.0.1:5432/lake_issue303_shared_20260928_final \
  ctest --test-dir build-shared --output-on-failure
tools/ui/test-token-gates.sh
```

`TMPDIR` selected this worktree's `.cache/tmp`; `LAKE_INSTANCE_KEK` used the documented test
fixture key. Each PostgreSQL test created its own isolated database and KEK.

```text
Static: 100% tests passed, 0 tests failed out of 419
Shared: 100% tests passed, 0 tests failed out of 419
sync-design-system: ok — 8 files match 38959d9966aa
build-tokens: ok — src/api/web_ui_tokens.inc matches the vendored tokens
token-gates: ok — generated output and snapshot changes were rejected
```

Both suites pass `Repositories.AnonymousCallersOnlySeePublicRepositories`, independent response
schema validation, all six `Experiments.*` cases and `lake-experiment-charts`. This includes actual
REST/MCP calls for all eleven operations and the page control/current-navigation/icon checks.
Each suite retains fourteen existing skips (optional browser/client checks and duplicate
PostgreSQL UI cases) and one disabled search benchmark; no Experiments case skips.

Native HTTP smoke checks returned `200` for server info, OpenAPI and the Experiments page, and
the generated OpenAPI listed all eleven operations. Interactive browser verification was not
available because the browser connection failed before opening a page. Linux proof is deferred:
the prescribed lane helper is unavailable. No Docker or local image build was used. These native
results do not claim the live-site training acceptance described above.
