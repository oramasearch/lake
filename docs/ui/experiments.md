# Train → Experiments

Experiments follows Runs in Train. `navigation_routes` owns `/train/experiments`,
`/train/experiment?study=`, `/train/experiment/run?run=` and
`/train/experiments/compare?runs=run-a,run-b`. Each selects Experiments with `aria-current="page"`.
Runs and Run reports link to their metric charts.

The list displays studies with state, objective, best complete-trial value, trial counts and
trial/cost budgets, plus tracked runs outside studies. Project and state filters apply to the
selected space. Starting a study displays its parameters and budgets in a confirmation dialog.
Study and trial stop buttons are available to writers; promotion requires space administration.
No page exposes a credential or contacts Zetta directly.

## Studies and Runs

A study displays objective history and its best-so-far line, normalized parallel coordinates,
parameter importance, trial states and a sortable leaderboard. The column picker includes trial
parameters and recorded final metrics. Confidence bounds come from the engine analysis. Each trial
links to its Run charts; two to eight selected trials can be compared. The latest eight trial
curves are drawn on the study page. All trial metrics remain available through their Run links,
and leaderboard metric values are loaded for every trial with at most four concurrent requests.
Transitioning out of running always refreshes final metrics before caching a terminal trial.

Single-run and comparison pages draw every metric grouped by its prefix. Runs keep consistent
colors, with dashed lines supplementing the palette. The X axis can be step, seconds since each
run's first recorded point, or wall time. Smoothing is an EMA with displayed weight 0–99%:
`smoothed = weight * previous + (1 - weight) * current`; its first value is the first raw point.
Missing run/step pairs remain absent. Log scale omits nonpositive plotted values while retaining
them in the data table. Drag selects an X region, Shift-drag pans, and keyboard-accessible buttons
zoom, pan and reset. The hover crosshair and legend show values. User-selected X bounds survive
live refreshes; changing the axis resets them.

The parameter table marks differing values and distinguishes missing values from null. Artifacts
include confusion matrices, ROC/PR, calibration, feature importance and sample prediction tables.
Every plot has its exact data available in a table; initial metric records remain readable when
JavaScript is disabled. User-controlled strings are escaped or assigned as text, including artifact
cells and embedded JSON. A DP run says “curves suppressed for differential privacy” and is excluded
from curve drawing and live metric requests.

## Controls and Capabilities

| Control or data region | Registered capability |
|---|---|
| Space/project/state filters and list | `list_experiments` |
| Start form and confirmation | `start_study` |
| Study status and trial links | `get_experiment_study` |
| Trial-state filter | `list_experiment_trials` |
| History, parallel coordinates, importance, sorting and columns | `get_experiment_analysis` |
| Single-run metrics, artifacts and presentation controls | `get_run_metrics` |
| Compare selection, overlays and parameter differences | `compare_runs` |
| Live toggle and polling | `tail_experiment_events` |
| Stop study or trial | `stop_study`, `stop_experiment_trial` |
| Promote button and confirmation | `promote_trial` |

Presentation controls transform already-authorized capability results locally. They declare the
source capability with `data-capability`; they do not create private API operations. Mutation
buttons are disabled until JavaScript has installed confirmation handling. Escape or Cancel in the
dialog sends no action. The same confirmation field and permission checks apply to REST/MCP and
the raw workspace action adapter. The [API contract](../api/experiments.md) lists all schemas and
examples, including the naming distinction from solution holdouts.

## Live Updates and Assets

The page follows `study-events.<study>` and `run-metrics.<run>` through Lake's bounded long-poll
capability. Events trigger durable-state refreshes; periodic refresh also repairs gaps when an
ephemeral topic is not present. At most eight Run topics and one Study topic are followed. Hidden
tabs and the Live toggle pause further polls. Transient failures show a reconnecting state.
An unsuccessful initial refresh also enters the reconnect loop.

uPlot 1.6.32 is vendored under `assets/experiments/`, with its MIT license and recorded SHA-256
digests. CMake embeds the library, its CSS, and Lake's chart controller/styles in the binary. Each
is served through a content-hashed `/assets/experiments/` URL with immutable caching; aliases use
`no-cache`. There is no chart CDN. The Lake overrides and canvas palette consume existing design
tokens and redraw for explicit or system theme changes.

## How to Verify and Change

Build static and shared using the authorized bridge Zetta client transport and run their complete
CTest suites with fresh local PostgreSQL connection databases. Run `tools/ui/test-token-gates.sh`.
`Experiments.*` covers every route's current navigation item and every rendered control's capability
mapping. `SchemaConformance.*` independently validates actual REST/MCP output schemas. The native
chart test verifies numerical behavior and event cursor continuation.

For interactive local review, run `LAKE_EXPERIMENT_BROWSER=1 build/tests/lake-tests
--gtest_filter=Experiments.EveryPageControlUsesACapabilityAndNavigationOwnsAllExperimentRoutes`.
The fixture prints the local URL and a fixture-only operator token. Sign in through `/login`,
start a study, observe trials and increasing metric points, compare two runs, change smoothing
and log scale, cancel promotion and then confirm it, and inspect both themes and narrow widths.
The fixture uses production Lake routing/registry/UI with a stateful fake workspace caller; it
does not claim to exercise a deployed trainer.

Keep chart transformations in `assets/experiments/experiments.js`, page markup in the pure
`web_ui_experiments` renderer and all data/actions in registered capabilities. Update the fixture,
control mapping tests and API docs together. Updating uPlot requires its version, license and
checksums to change together and both asset/cache and browser verification to pass.

Deployment acceptance must separately start a real study on the live site, observe real trial
curves, compare two real runs with smoothing/log scale, and exercise promotion confirmation after
the reviewed Lake change is deployed. This work order opens a PR without merging or deploying it.

Study deletion is deferred to [Zetta #1158](https://github.com/oramasearch/zetta/issues/1158),
as agreed for Lake #303. The authorized engine contract has no deletion operation. The page
therefore has no delete control, including no disabled placeholder, and Lake implements no
local-only hiding or deletion substitute. Add the registered REST/MCP operation and its UI control
together once the engine contract is available.
