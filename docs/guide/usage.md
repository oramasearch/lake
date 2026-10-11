# Usage and Metering

**Organization → Usage** shows what your workspaces store and use over time: storage by kind,
transfer, build and run compute, speech-to-text, API and MCP calls, model predictions, model
tokens, and the instance and volume time of your services, stores and apps. Choose the workspaces
and the period, compare it with the period before, chart it by workspace or by kind, set limits that
warn as you near them, and export it as CSV. **Repository Usage by Hour** keeps the exact
repository transfer and build time, the repository storage limit and, when the lake has prices set,
an estimated charge.

Usage is a measurement, not a bill: the dashboard shows no prices. On Orama Cloud, **Organization →
Billing** turns it into a monthly bill on your plan; an enterprise install has no billing.

## Concepts

| Term | What it is |
|---|---|
| **Dimension** | One kind of usage with one unit: **Storage** (bytes), **Transfer** (bytes), **Build Compute** and **Workspace Run Compute** (time), **Speech to Text** (time), **API and MCP Calls**, **Model Predictions**, **Model Tokens**, **Service and App Instances** (instance-hours) and **Service and Store Volumes** (GiB-hours). |
| **Workspace usage** | Each workspace keeps its own usage in the workspace itself, an hour at a time, for 400 days. Usage adds up the workspaces you choose. |
| **Storage** | The latest reading in a period: repositories, container images, workspace data, transcripts and drive files, read every fifteen minutes. |
| **Instance time** | How long each service and app ran ready, times its instances or replicas. Your workspace's own engine is a service, and counts. |
| **Volume time** | The size of each running service's and store's volumes, times how long it ran. |
| **Model tokens** | The tokens an agent reports it spent, by agent, run and model, and the tokens a speech-to-text model reports for a transcript. |
| **Limit** | A ceiling on one dimension with a warning percentage. The storage limit is enforced on new repository writes; every other limit warns and refuses nothing. |
| **Repository storage limit** | The storage limit: above it, new repository writes are refused; what is stored stays readable. |
| **Estimated charge** | On Repository Usage by Hour only: transfer and repository build time at the prices the lake operator configured. |

## Open Usage

1. Open **Organization → Usage**. Every workspace is chosen; press one to see only it, press more
   to add them, or press **All workspaces**. The choice stays in the address (`?ws=`) and in this
   browser, as on Home's Dashboard.
2. Choose the period: **24 hours**, **7 days**, **30 days**, **This month** or **12 months**.
3. Read the figures, one per dimension, each with its change against the period before (this month
   is compared with all of last month). Press a figure to chart it.
4. **Over Time** charts the chosen dimension, stacked **By workspace** or **By kind**. The chips
   above it switch the dimension.
5. Beside the chart: the breakdowns of the chosen dimension (by kind, and by agent, run, model or
   service where they apply), **Limits** with a meter for each, **Not Measured Yet**, and a link to
   **Repository Usage by Hour**.
6. **By Workspace** lists every dimension's figure per workspace, or why a workspace could not be
   read.

The same reading, for one account and all its workspaces:

- REST: `GET /v1/usage_over_time?customer_id=<customer id>&from=2026-10-01T00:00:00Z&to=2026-11-01T00:00:00Z&granularity=day`.
- MCP: `usage_over_time` with the same input.

For chosen workspaces and dimensions, by hour:

- REST: `GET /v1/usage_over_time?customer_id=<customer id>&workspace_ids=<workspace id>&dimensions=calls,storage&from=2026-10-10T00:00:00Z&to=2026-10-11T00:00:00Z&granularity=hour`.
- MCP: `usage_over_time` with

```json
{"customer_id": "<customer id>", "workspace_ids": ["<workspace id>"], "dimensions": ["calls", "storage"], "from": "2026-10-10T00:00:00Z", "to": "2026-10-11T00:00:00Z", "granularity": "hour"}
```

`granularity` is `hour`, `day` or `month`; `from` is rounded down and `to` up to whole UTC periods.
A reading covers at most 100 days of hours, or three years of days or months. A workspace of
another account is refused, never read.

## Export Usage as CSV

On **Organization → Usage**, press **Export CSV**. The file has one row per dimension, breakdown
(the total, each workspace, each kind, agent, run, model or service) and period, in the dimension's
own unit (bytes, seconds, calls, tokens, instance-seconds, GiB-seconds), so a spreadsheet adds them
up exactly.

## Set a Usage Limit

You need `admin` on the account.

1. On **Organization → Usage**, press **Set a limit**.
2. Choose **What to limit**, enter the limit in the unit shown (GiB for storage, hours for compute
   and speech, instance-hours, GiB-hours, or a count), and **Warn from** a percentage (80 by
   default).
3. Press **Save limit**. Leave the limit empty or 0 to remove it.

- REST: `POST /v1/set_usage_limit` with
  `{"customer_id": "<customer id>", "dimension": "calls", "limit": 100000, "warn_percent": 80}`.
- MCP: `set_usage_limit`.

Read the limits, with what is used and each one's state (`ok`, `warning`, `over`):

- REST: `GET /v1/usage_limits?customer_id=<customer id>`.
- MCP: `usage_limits`.

A limit other than storage counts this UTC calendar month and warns: Usage shows **Near a Limit**
and the meter turns red past it. Nothing is refused.

## Set the Repository Storage Limit

The storage limit is the repository storage limit. Set it with **Set a limit** and **Storage**, or
on **Repository Usage by Hour** with **Set storage limit**.

- REST: `POST /v1/set_storage_quota` with
  `{"customer_id": "<customer id>", "quota_bytes": 10737418240}`.
- MCP: `set_storage_quota`.

The value overrides the lake's default limit for that account; `0` removes the limit.

## What Happens over the Limit

- A push or upload that would take repository storage past the limit is refused as busy (HTTP 429),
  and repository metadata writes are throttled while storage is at or above it.
- Everything already stored stays complete and readable. Lake never replaces content with a pointer
  or a smaller copy.
- Usage shows a warning and the storage meter turns red.

To write again, delete what you no longer need, or ask an admin of the account to raise the limit.

## Usage of One Agent, App, Service or Model

- An agent's page (**Train → Agents**, then the agent) shows its **Model tokens** this month.
- An app's page (**Ship → Apps**, then the app) shows its **Instance time** this month.
- **Ship → Services** shows each service's instance and volume time this month.
- A model's page (**Ship → Models**, then the model) shows its **Predictions answered** this month.

Each card's **Usage** button opens the dashboard on that workspace and dimension. Home's
**Dashboard** has a **Usage** button for the workspaces chosen there.

## Report an Agent's Model Tokens

An agent run reports the tokens it spent with its own credential (a task, launch or schedule
credential, or a library agent's bus credential with its task):

- REST: `POST /v1/record_model_usage` with
  `{"model_name": "gpt-5", "input_tokens": 1200, "output_tokens": 300}`.
- MCP: `record_model_usage`.

Lake counts them in the workspace the run belongs to. A person's own sign-in or token is refused.

## Repository Usage by Hour

**Repository Usage by Hour** (from Usage, or an organization's **Usage** tab) shows what the account
stores now and the exact repository transfer and build time hour by hour: **Stored**, **Transfer**,
**Builds** and **Run time**, the **Repository storage limit** and the **Estimated charge**.

- REST: `GET /v1/metering_usage?customer_id=<customer id>`.
- MCP: `metering_usage` with the same input.

Narrow the hours with `from` and `to` (inclusive UTC hours) and `limit` (1 to 10,000, default 744):

- REST: `GET /v1/metering_usage?customer_id=<customer id>&from=2026-09-01T00:00:00Z&to=2026-09-30T23:00:00Z&limit=744`.

Prices are set by the lake operator, for inbound and outbound transfer per GiB and for compute per
second; only transfer and repository build time are priced, and with no price set the page says
there is no charge to show. How usage is billed is not decided.

## The Operator's View

The lake operator's **Administration → Usage** lists every account's usage for the period, with the
totals across accounts, for capacity planning. **Open** shows one account's Usage with all its
workspaces.

## What Is Not Measured Yet

**Not Measured Yet** names, in plain sentences, what a reading could not include:

- CPU and memory of services, stores and apps: only instance and volume time is measured;
- model tokens an agent has not reported (agents do not report them yet);
- speech-to-text for dictation outside a transcript;
- model files and indexes inside a workspace, whose size is not reported yet;
- a workspace that is not running, or could not be read: never counted as zero, it is named
  instead.

Usage before this measurement started is not in the charts.

## Permissions

| Call | Needs |
|---|---|
| `usage_over_time`, `usage_limits`, `metering_usage` | `read` on the account (any organization member, for the organization's account) |
| `set_usage_limit`, `set_storage_quota` | `admin` on the account |
| `record_model_usage` | the agent run's own credential |

Each read and each limit change is recorded in the central audit log; an agent's report is
recorded in its workspace's audit.

## Limits

- Calls, predictions, speech and tokens reach the workspace within about ten seconds; storage and
  instance time are read every fifteen minutes.
- The usage series are approximate: a server that stops abruptly loses the last few seconds, and
  rarely a write is counted twice. Repository transfer and build time are exact.
- Image build time runs from when the build was queued to when it finished.
- When an account has several workspaces, each repository's bytes count in its own workspace, so a
  file two workspaces' repositories share counts in both.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| **Start your first workspace** | Usage is kept per workspace; create one. |
| `invalid_input` at `/workspace_ids/0` | That workspace is not a live workspace of the account. |
| `invalid_input` at `/to` | `to` is not after `from`, or the period is too long for the granularity. |
| `unauthorized` | You have no grant on that account, or (for `record_model_usage`) the credential is not an agent run's. |
| **Near a Limit** | A dimension passed its warning percentage this month. Raise the limit or use less. |
| Pushes answer 429 busy | Repository storage is at the limit. See [What Happens over the Limit](#what-happens-over-the-limit). |
| A workspace shows **Not read** | It is not running or could not be read; its figures are left out, not counted as zero. |

## Related Pages

- [Organizations, Members and Teams](/docs/organizations)
- [Workspaces](/docs/workspaces)
- [Services and Storage](/docs/services)
- [Agents](/docs/agents)
- [Activity and Audit](/docs/activity)
