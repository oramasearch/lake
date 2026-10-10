# Solutions, Methodologies and Opportunities

Three Design pages help you decide what to build before you build it:

- **Design → Solutions**: Orama's catalogue of solutions. Each module answers one business problem
  end to end. Start one and it becomes a project in your workspace.
- **Design → Methodologies**: the modelling methods your workspace's engine knows, what each needs
  and makes, and whether it runs there today. Solutions compose them.
- **Design → Opportunities**: what your data could be used for. Auto-Suggest reads your datasets and
  recommends what to build from the solutions catalogue.

Every REST and MCP call on this page names `customer_id` (the id of the account that owns your
workspaces, yours or your organization's) and `workspace_id`; [Getting Started](/docs) says where
to find them.

## Concepts

| Term | What it is |
|---|---|
| **Solution** | A module from Orama's catalogue: the methodologies it composes, the decision it drives, and how its return is measured. Its id looks like `S-1`. |
| **Level** | How far a solution goes, from 1 to 4; each solution lists its own levels. |
| **Methodology** | A modelling method the engine runs, such as churn classification, segmentation, forecasting or anomaly detection. Its id looks like `M-3`. |
| **Foundation** | What a solution project is built on: the snapshot date, the entity, the dataset and its label and feature columns, the methodology roles, the exclusion predicates, delivery and economics. |
| **Exclusion predicate** | A column that rules an entity out of an action, such as an open complaint. A missing one is called out and blocks Run; it is never assumed to be false. |
| **A1 decision** | A decision that needs a person's approval before its action may go ahead. |
| **Opportunity** | A recommendation from Auto-Suggest: an intent your data supports, matched to the catalogue. |

## Browse the Solutions

Open **Design → Solutions**. The figures count the solutions in the catalogue, how many run on
this workspace's engine (**Run here**) and how many are **In design** (sketches that are not yet a
module). Search filters the cards. **Modules** come first, then **In Design**.

Open a solution to read **What It Does**, its **Levels**, and its status: whether it runs here and,
when it does not, where that is tracked.

Through the API, the catalogue is `list_solutions` and one solution is `get_solution`, both inside
`workspace_query`:

`POST /v1/workspace_query`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "get_solution", "input": {"solution": "S-1"}}
```

An engine without the solution registry says so on the page instead of listing anything.

## Start a Solution

Starting a solution creates a project, then the Project Agent proposes its foundation for you to
review. Nothing else is created until you confirm.

1. On the solution, press **Start this solution** (or **Start** in the side column).
2. In **Start This Solution**, fill in:
   - **Project name**;
   - **Level**;
   - **Dataset**: the data the solution works on;
   - **Delivery**: **Webhook**, **Salesforce connector** or **HubSpot connector**, with the
     **Webhook URL or connector endpoint** and the **Owner** who receives the actions;
   - **Value per outcome**, **Investment** and **Currency**. These are your inputs: lake supplies
     no defaults.
3. Press **Start**. The project opens and the Project Agent reads the dataset (*Foundation
   proposal: the Project Agent is reading the dataset…*).

`POST /v1/start_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "collections", "solution": "S-1", "level": 1, "dataset": "datasets.accounts", "delivery": {"kind": "webhook", "endpoint": "https://example.com/hooks/actions", "owner": "ops@example.com"}, "roi_model": {"value_per_outcome": 120, "investment": 25000, "currency": "USD"}}
```

The answer carries the Project Agent's `task_id`. The MCP tool is `start_solution`.

## Review and Confirm the Foundation

On the project's **Solution** tab, **Foundation Proposal** shows the exact foundation that will be
sent to the engine: its snapshot date, entity, label and feature columns, methodology roles and
exclusion predicates. Delivery and economics are your inputs from Start.

1. Read it, and edit the **Foundation (JSON)** where it is wrong.
2. If the dataset is missing an exclusion predicate, the page warns *Actions will remain blocked*
   and names it. It is not treated as false.
3. Tick *I reviewed this snapshot, these roles, predicates, delivery settings and economics.*
4. Press **Confirm and create solution**.

`POST /v1/confirm_solution`

```json
{"task_id": "<task id>", "workspace_id": "<workspace id>", "edits": {}}
```

`edits` is applied as a JSON Merge Patch over the proposed foundation: put only the fields you
changed in it, or send it empty (or leave it out) to confirm the proposal as it is. Only the person who started the solution can confirm it. The answer says which
predicates are missing and whether actions are blocked.

## Operate a Solution Project

After confirmation the project's **Solution** tab shows:

- **Solution Stages** and **Run**. Run starts the solution's pipeline (with an optional delivery
  token used for that run only). Run is blocked while an exclusion predicate is missing. The
  runs table shows each run's status, stage and error, and refreshes every 3 seconds while one is
  active.
- **Decision Policy**: the current policy. To change it, edit the **Configuration (JSON)**, give a
  **Reason**, tick that you approve the proposed version, and press **Create policy version**.
- **Experiment and Holdout**: the configured share, assignments and holdout assignments, and their
  balance.
- **Actions and A1 Queue**: decisions, delivered actions and held ones, and the A1 decisions
  waiting for approval. **Approve A1** asks first: approving changes the decision state; it does
  not deliver an action by itself.
- Outcomes: record what happened to an action with **Outcome**, **Value**, **Currency** and
  **Occurred at**, then **Record**. Recording updates the audited ledger and the ROI evidence.
- **Monitor** and **ROI Readout**: return on investment, net and gross value, or why ROI cannot be
  claimed yet.

These are workspace actions inside `workspace_act`: `run_solution`, `update_policy`,
`approve_decision` and `record_outcome`; the reads are `get_policy`, `get_experiment`,
`list_ledger`, `get_monitor` and `get_roi_readout` inside `workspace_query`. For example, approving
one A1 decision:

`POST /v1/workspace_act`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "approve_decision", "input": {"decision_id": "<decision id>"}}
```

## Methodologies

Open **Design → Methodologies**. The figures count the methodologies the engine knows, how many
**Run here** today and the families. The catalogue lists 21 methodologies as cards grouped by
family; search filters them. A card that does not run here names its tracking issue.

Open one to see what it needs, what it makes, how it is measured, the **Solutions That Use It**
and, when it runs here, **Build a pipeline with it**, which opens Projects: open a project and
choose the methodology under Build a Pipeline ([Projects and Pipelines](/docs/design#build-a-pipeline-from-a-sentence)).

Through the API it is `list_methodologies` inside `workspace_query`:

`POST /v1/workspace_query`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "list_methodologies", "input": {}}
```

Each entry carries `runnable`. An engine that predates the registry answers `unknown_capability`;
lake then treats it as running only churn classification (`M-1`), and the page says *No
Methodology Registry Yet*.

## Opportunities

**Design → Opportunities** keeps the recommendations Auto-Suggest makes from your data until you
act on them.

1. Press **Scan my workspace** (or **Scan again** after a scan). Auto-Suggest reads your workspace's
   datasets and their columns and matches them to the solutions catalogue. It runs in the
   background and changes nothing; you can leave the page. While it works the page says *Scanning
   your workspace…* and checks again every 3 seconds.
2. The results are grouped:
   - **Recommended**: each card names the intent and whether the methodology it needs runs today.
   - **Not Yet**: what your data does not support yet, and why.
   - **Made into Projects** and **Dismissed**.
3. On a card, press **Create project**, then **Create project draft** in the confirmation. Or tick
   several cards, tick *Create a project draft for each selected Opportunity*, and press **Create
   project drafts**. Nothing runs until you approve a plan.
4. Press **Dismiss** on a card you do not want.

The scan is an Auto-Suggest turn:

`POST /v1/agent_turn`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-autosuggest", "kind": "discover", "message": "What could this workspace build from its data?"}
```

Then list, create and dismiss:

`GET /v1/list_opportunities?customer_id=<customer id>&workspace_id=<workspace id>&task_id=<task id>`

`POST /v1/create_projects_from_opportunities`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "opportunity_ids": ["<opportunity id>"]}
```

`POST /v1/dismiss_opportunity`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "opportunity_id": "<opportunity id>"}
```

When Auto-Suggest is not running on this lake, the page says *a new scan is not available*;
earlier Opportunities stay listed. **Design → Agent Library** shows which agents run
([Agents](/docs/agents)).

## Permissions

| To | You need, on the account that owns the workspace |
|---|---|
| Browse solutions, methodologies and opportunities | `read` |
| Start or confirm a solution, run it, change its policy, approve A1 decisions, record outcomes | `write` |
| Scan, create project drafts, dismiss | `write` |

Only the person who started a solution (or the operator) can confirm its foundation. You hold
`admin` on your own workspaces; in an organization, members read unless they were granted `write`.

## Limits

- Delivery is a webhook, Salesforce or HubSpot; its endpoint and owner are required.
- Only a solution that runs on this engine, or one in preview, can be started; the others are
  listed with where their work is tracked.
- Missing exclusion evidence blocks Run until the dataset carries it.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| `invalid_input` at `/delivery/...` or `/roi_model/...` | A delivery or economic input is missing or empty; all are required. |
| *Run is blocked because an exclusion predicate is missing* | Add the missing column to the dataset, or map it in the foundation, and confirm again. |
| *ROI cannot be claimed yet* | The readout gives the reason under it. Keep running and recording outcomes. |
| `not_approved` (409) | Approve the plan or the decision first. |
| *The scan did not finish* | Auto-Suggest failed; the message says why. Scan again. |
| `unknown_capability` from `list_methodologies` | The workspace engine predates the registry; it runs churn classification only. |
| `unauthorized` | You have `read` but not `write`, or you did not start this solution. |

## Related Pages

- [Projects and Pipelines](/docs/design)
- [Datasets and Loading Data](/docs/load-data)
- [Agents](/docs/agents): the Agent Library
- [Train a Model](/docs/train)
- [Ask Orama](/docs/ask-orama)
