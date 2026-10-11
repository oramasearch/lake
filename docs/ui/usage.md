# Usage

**Status: implemented (lake#564), not deployed.** Organization → Usage (`/usage`) is a dashboard of
what the chosen workspaces store and use over time, read from each workspace's own usage
([../api/metering.md](../api/metering.md#usage-over-time)). It shows no price: prices and
invoices are Organization → Billing's, only where the lake bills ([billing.md](billing.md)).

## Pages

| Route | File | Shows | Capabilities |
|---|---|---|---|
| `/usage?ws=&range=&dimension=` | `frontend/src/pages/usage/UsagePage.tsx` | the dashboard, below | `usage_over_time` (this period and the one before, per account), `usage_limits`, `set_usage_limit` |
| `/usage?customer_id=` | the same | the operator's view of one account and all its workspaces | `list_workspaces` and the above |
| `/usage/accounts?range=` | `frontend/src/pages/usage/AccountsPage.tsx` | Administration → Usage: every account's usage for the period, the totals across accounts, a search, each account's figures per dimension, and **Open** to its dashboard; the first 200 accounts are read | `list_customers`, `usage_over_time` |
| `/metering?customer_id=` | `frontend/src/pages/admin/MeteringPage.tsx` | Repository Usage by Hour: the exact repository transfer and build time, the repository storage limit and the estimated charge, unchanged; the dashboard links it | `metering_usage`, `set_storage_quota` |

The rail's **Usage** item is `/usage` for a person (Organization) and `/usage/accounts` for the
operator (Administration). `/usage` owns `/metering` in the rail.

## The Dashboard

Top to bottom:

- **Header.** Eyebrow Organization (Administration for the operator), title Usage, one sentence;
  **Every account** (operator), **Export CSV** and **Set a limit** (when the chosen workspaces belong
  to one account).
- **Workspaces.** Home's multi-select (`components/WorkspaceToggles.tsx`, shared with the Dashboard):
  **All workspaces** and one toggle per workspace, kept in `?ws=` and remembered in this browser
  under `lake-usage-workspaces`. The selection logic is `lib/workspaceSelection.ts`.
- **Period.** **24 hours** (by hour), **7 days**, **30 days**, **This month** (by day; the default)
  and **12 months** (by month), in `?range=`. Each is compared with the period before it; this month
  with all of last month (`windows`, `pages/usage/usage.ts`).
- **Near a Limit.** One warning banner per limit at or past its warning percentage.
- **Figures.** One tile per dimension: the total (a reading's latest value for storage), its unit
  in words (`formatValue`: CPU in vCPU-minutes or -hours, RAM and GPU memory in GiB-minutes or
  -hours, GPU in GPU-minutes or -hours), and its change against the last period with an arrow. Pressing a tile
  charts it (`?dimension=`).
- **Over Time.** A uPlot chart (`pages/usage/UsageChart.tsx`, as Train and Ship's charts) of the
  chosen dimension, stacked **By workspace** or **By kind**: the six largest bands and Other
  (`stackSeries`), in lake's colours, redrawn when the theme changes; the legend shows each band's
  own value. Chips switch the dimension.
- **Breakdowns.** Beside or below the chart: the chosen dimension by kind, and by agent, run, model,
  workload or model (**By Workload or Model**) and GPU class (**By GPU Class**, lake#643; "Unknown
  class" for a GPU the operator could not name) where the reading has them, each with its total and
  share.
- **Limits.** One meter per limit (used of limit, percent, and what it does at its line, `limitNote`:
  storage refuses new repository writes, model tokens refuse agents' model calls through the
  [model gateway](../api/model-gateway.md#limits), every other limit warns), red past the limit;
  **Set a limit** opens a dialog: what to limit, the limit in a person's unit (GiB, hours,
  vCPU-hours, GiB-hours, GPU-hours, instance-hours or a count, `toLimit`), and the warning
  percentage. Empty or 0 removes
  it.
- **Not Measured Yet.** The reading's `not_measured` sentences, as a quiet banner.
- **Repository Usage by Hour.** A card linking `/metering` for the account.
- **By Workspace.** Every dimension's figure per workspace, sortable, or why a workspace was not read.

Several accounts' workspaces are read one account at a time and added up (`mergeReadings`). The
dashboard never names the account itself to a person; with several, a limit names its account.

## Export CSV

**Export CSV** downloads `lake-usage-<from>-<range>.csv`: `dimension, unit, breakdown, name, period,
value`, one row per dimension, breakdown (total, workspace, kind, agent, launch, model, object,
class) and
period, values in the dimension's own unit (`toCsv`).

## Usage of One Thing

`pages/usage/UsageThisMonth.tsx` is a side card, **Usage This Month**, on:

| Page | Figure | Read |
|---|---|---|
| Train → Agents → an agent | **Model tokens**, by the agent's name or id | `usage_over_time` `model_tokens`, `by_agent` |
| Ship → Apps → an app | **CPU**, **RAM**, **GPU** and **Instance time** | `cpu`, `ram`, `gpu`, `instances`, `by_object` |
| Ship → Models → a model | **Predictions answered** | `predictions`, `by_object` |

Each reads its workspace for this month and links the dashboard on that workspace and dimension.
Ship → Services adds a **This Month** column (CPU, then RAM and volume time per service, from one
`usage_over_time` read of the account; lake#643), and Home's Dashboard a **Usage** button for its selection.

## Phone

At 390 px the figures stack one to a row, the Split stacks, the chart takes the card's width, and the
tables scroll in their own box.

## Verify

`frontend/tests/usage.test.ts`: windows and their previous periods, units, changes, two accounts
added up, the stacked series with Other, the CSV, limits in a person's units and what each does,
links, one thing's
usage, and the workspace selection. `npx tsc --noEmit -p .`, `npm test` and `npm run build` in
`frontend/`.

## Change

A dimension the server adds appears as a figure, a chip and a column with no change here; give its
unit a case in `formatValue` and `limitScale` if it is new, and its kinds names in `KIND_NAMES`.
