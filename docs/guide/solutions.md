# AI Solutions, Methodologies and Opportunities

Three Design pages help you decide what to build before you build it:

- **Design → Solutions**: Orama's catalogue of ready-made AI solutions, and the solutions you write
  yourself. Each answers one business problem end to end. Start one and it becomes an AI project in
  your workspace, with its pipeline.
- **Design → Methodologies**: the modelling methods your workspace can run: Orama's, built into the
  engine and read-only, and your own, written in Python. What each needs and makes, and whether it
  runs there today. Solutions compose them.
- **Design → Opportunities**: what your data could be used for. Auto-Suggest reads your datasets and
  recommends AI projects to build from the solutions catalogue.

Every REST and MCP call on this page names `customer_id` (the id of the account that owns your
workspaces, yours or your organization's) and `workspace_id`; [Getting Started](/docs) says where
to find them.

## Concepts

| Term | What it is |
|---|---|
| **Solution** | An AI solution: the business problem it answers, the decision it drives, how its return is measured and the methodologies it composes. **Orama's** come from the catalogue, are read-only, and have ids like `S-1`. **Yours** are written in your workspace and can be edited. |
| **Level** | How far a solution goes, from 1 to 4; each solution lists its own levels. |
| **Methodology** | A modelling method, such as churn classification, segmentation, forecasting or anomaly detection. **Orama's** are built into the engine, are read-only and have ids like `M-3`. **Yours** are Python in a repository of your workspace, with ids like `W-0a1b2c3d`. |
| **Foundation** | What a solution project is built on: the snapshot date, the entity, the dataset and its label and feature columns, the methodology roles, the exclusion predicates, delivery and economics. |
| **Exclusion predicate** | A column that rules an entity out of an action, such as an open complaint. A missing one is called out and blocks Run; it is never assumed to be false. |
| **A1 decision** | A decision that needs a person's approval before its action may go ahead. |
| **Opportunity** | A recommendation from Auto-Suggest: an intent your data supports, matched to the catalogue. |

## Browse the Solutions

Open **Design → Solutions**. Every card is marked **Orama** (from Orama's catalogue, read-only) or
**Yours** (written in this workspace, editable). The figures count Orama's solutions, yours, and how
many of Orama's run on this workspace's engine (**Run here**). Search filters the cards; the
**Orama** and **Yours** chips keep one kind, and the run-state chips (**Runs today**, **Not yet**,
**In design**) filter Orama's. **Yours** come first, then **Orama's Modules** and **Orama's, in
Design** (sketches that are not yet a module).

Open one of Orama's to read **What It Does**, its **Levels**, and its status: whether it runs here
and, when it does not, where that is tracked.

Each solution has an icon: a small picture of what it does, with an amber accent. Each methodology
has an icon with a blue accent, and a solution's icon reuses the picture of the methodology it is
built on. Your own solutions get an icon too:

- A duplicate of one of Orama's solutions keeps the original's icon.
- Otherwise your solution uses the icon of its first methodology, or of the methodology you marked
  as primary.
- A solution with no methodology shows the first letter of its name.

To see every icon, open **Design → Catalogue Icons** (`/design/icons`).

Through the API, `list_solutions` lists both kinds, each with `source` (`orama` or `workspace`) and
`editable`, and `get_solution` reads one:

`GET /v1/list_solutions?customer_id=<customer id>&workspace_id=<workspace id>`

`GET /v1/get_solution?customer_id=<customer id>&workspace_id=<workspace id>&id=S-1`

Add `source=orama` or `source=workspace` to `list_solutions` to keep one kind. When the workspace's
engine does not list Orama's solutions, the page says so and lists yours; the answer's
`catalogue.available` is `false`.

## Create Your Own Solution

Write your own when the catalogue does not cover your problem.

1. On **Design → Solutions**, press **New solution**.
2. In **New Solution**, fill in:
   - **Name**;
   - **The business problem** it answers;
   - **The decision it drives**;
   - under **How Its Return Is Measured**, the **Metric**, and optionally its **Baseline** (where it
     stands today) and **Target** (where it should go);
   - under **What It Composes**, the **Methodologies**: ids separated by commas, with the level each
     starts at after a colon when above 1, such as `M-1, M-3:2`. The help lists the methodologies
     this workspace's engine knows;
   - optionally a **Pipeline template or reference** (`project/pipeline` links to that pipeline) and
     **Notes**.
3. Press **Create solution**. Your solution opens.

`POST /v1/create_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Late payments", "problem": "Invoices are paid late and cash runs short at month end.", "decision": "Which customers collections calls first each morning.", "measurement": {"metric": "days sales outstanding", "baseline": "52", "target": "40"}, "methodologies": ["M-1", {"id": "M-3", "minimum_level": 2}], "pipeline": "collections/late-payers"}
```

The answer is `{solution}`, with its `id`. The MCP tool is `create_solution`. Your solutions are kept
in your workspace and are not seen from your other workspaces.

## Edit, Duplicate and Delete

On one of your own solutions:

- **Edit** opens the same fields in a drawer; press **Save**.
- **Duplicate** makes a copy named *<name> (copy)* and opens it.
- **Delete** asks you to type the solution's name, then deletes it. It cannot be restored. Projects
  you started from it are kept.

Orama's solutions are read-only. To change one, open it and press **Duplicate to edit**: it becomes
one of yours, with its problem, decision, measured outcome, methodologies and pipeline order filled
in, and notes that say where it came from. Its baseline and target are yours to fill in.

`POST /v1/update_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<solution id>", "measurement": {"metric": "days sales outstanding", "baseline": "52", "target": "38"}}
```

The fields you send replace theirs; the others are kept.

`POST /v1/duplicate_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "S-1", "name": "Our churn"}
```

`POST /v1/delete_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "<solution id>"}
```

Editing or deleting one of Orama's is refused with `forbidden` (403), whatever your permissions,
and the message says to duplicate it instead.

## Start Your Own as a Project

On one of your own solutions, press **Start as a project** (or **Start as a project** in the side
column). In the drawer, give the **Project name**, choose the **Methodology** its first pipeline
uses (one the solution composes; the first by default) and optionally a dataset under **From
data**, then press **Start**. Lake creates the project with your solution's problem and decision,
and builds its first pipeline from the decision with that methodology. The project opens (or, with
a dataset, the new pipeline at its Run form).

A solution of your own does not get the governed foundation Orama's do (policy, holdout experiment
and ROI readout): the engine composes those only for its own catalogue solutions. If the first
pipeline cannot be built, the project is kept and the page says why; press **Build pipeline** on the
project to try again ([AI Projects and Pipelines](/docs/design#build-a-pipeline-from-a-sentence)).

## Start One of Orama's Solutions

Starting one of Orama's solutions creates a project, then the Project Agent proposes its foundation for you to
review. Nothing else is created until you confirm.

1. On one of Orama's solutions, press **Start this solution** (or **Start** in the side column).
2. In **Start This Solution**, fill in:
   - **Project name**;
   - **Level**;
   - **Dataset**: the data the solution works on;
   - **Delivery**: **Webhook**, **Salesforce connector** or **HubSpot connector**, with the
     **Webhook URL or connector endpoint** and the **Owner** who receives the actions;
   - **Value per outcome**, **Investment** and **Currency**. These are your inputs: Lake supplies
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

`POST /v1/approve_decision`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "decision_id": "<decision id>"}
```

## Methodologies

Open **Design → Methodologies**. The figures count the methodologies the engine knows, how many
**Run here** today and the families. The catalogue lists 21 methodologies as cards grouped by
family; search filters them. A card that does not run here names its tracking issue.

Open one to see what it needs, what it makes, how it is measured, the **Solutions That Use It**
and, when it runs here, **Build a pipeline with it**, which opens Projects: open a project and
choose the methodology under Build a Pipeline ([AI Projects and Pipelines](/docs/design#build-a-pipeline-from-a-sentence)).

Through the API it is `list_methodologies` inside `workspace_query`:

`POST /v1/list_methodologies`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>"}
```

Each entry carries `runnable`. An engine that predates the registry answers `unknown_capability`;
Lake then treats it as running only churn classification (`M-1`), and the page says *No
Methodology Registry Yet*.

## Your Own Methodologies

A methodology of your own is Python in a repository of your workspace, so it is versioned and
reviewed like any other code. It is three files:

- `methodology.json` says what it needs (tabular data, text, a time series, a graph, …; a target or
  none; required features), its **parameters** with their types and defaults, the **metrics** it
  reports and the resources it may use;
- `methodology.py` defines `fit(data, params)`, which returns what it learned, `predict(artifact,
  data)`, one prediction per row, and `score(artifact, data)`, its metrics (and `explain` where it
  can);
- `requirements.lock` pins the packages it installs, each with its hash; an empty file means the
  standard library only.

Every run reports each metric twice: in sample, on the rows it trained on, and held out, on the
rows it did not. Quote the held-out figure.

### Create One

1. On **Design → Methodologies**, press **New methodology**.
2. Give it a **Name**, say **What it should do**, and optionally its **target** (a yes-or-no
   outcome, one of several categories, an amount, or no target to find groups).
3. Press **Ask Orama**. Ask Orama writes a first version for your description: the manifest,
   `methodology.py` and the lock file, with every package it uses pinned and hashed. Lake checks
   the files the way Validate does before running anything, and only then shows them, marked
   **Written by Ask Orama**, for you to read. If Ask Orama is not running on your Lake, or its draft
   does not pass those checks, Lake shows its own template for the kind of problem you described,
   marked **Lake's template**, and says why.
4. Read the files, then press **Create methodology**. Lake commits them to a new private
   repository, `methodology-<name>`, and the methodology's page opens, marked **Yours**, *Not
   validated*. Nothing is saved before you press Create.

Through the API:

`POST /v1/create_methodology`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "Late payers", "description": "Which customers will pay their invoice late?"}
```

Send `files` (`{path, contents}`) to commit your own, or `repository_id` to use code already in a
repository of the workspace. To have Ask Orama write the files, send it a `methodology_draft` turn
(`agent_turn` with `agent: "agent-ask-orama"`, `kind: "methodology_draft"`, your description as the
`message`), follow it with `get_agent_task`, then read the checked draft with
`draft_methodology` and its `task_id` ([methodologies](/docs/api/methodologies#drafts)).

### Edit, Validate and Train

- **Code**: the page shows its files; edit one and press **Commit**. Each commit is a new revision
  of its repository, and a new revision must be validated again before it runs. Code pushed to the
  repository another way is followed with `update_methodology` and `revision`.
- **Validate** checks it first without running anything (the manifest, `fit`, `predict` and
  `score`, the lock file) and lists what is missing. When that passes, Lake builds its image and
  runs it on a small sample in its sandbox; the result marks it **Usable** or lists what failed.

  `POST /v1/validate_methodology`

  ```json
  {"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "W-0a1b2c3d"}
  ```

- **Train** (once usable) runs it on a dataset you choose, with its target column and parameters.
  The model it trains appears in **Train → Models**; the run, its held-out metrics and its log are
  on the page under **Runs**. A workspace runs a few methodology runs at a time (two unless your
  Lake is set otherwise); a run started past that is **Queued**, with the reason beside it, and
  starts when one ends. A run that asks for GPUs also waits while no GPU node of its class exists.

  `POST /v1/run_methodology`

  ```json
  {"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "W-0a1b2c3d", "dataset": "sales.payers", "target": "late"}
  ```

- **Use it in a pipeline**: once usable, it is listed under **Methodology** in Build a Pipeline,
  and the pipeline's runs run its code. Name it among a solution's methodologies like any other.
- **Edit** changes its name, description (written to its manifest) and whether its runs may reach
  the internet. **Duplicate** copies it, code included, into a new repository. **Delete** removes it
  from the workspace and stops its runs; its repository and models stay.

### Orama's Stay Read-Only

Orama's methodologies are marked **By Orama** and cannot be edited or deleted. Press **Duplicate to
edit** on one to start one of your own: its description and parameters are copied, with a first
version of the Python to change, since Orama's run natively in the engine.

### Where It Runs

Your code runs as untrusted code, in its own container, with its pinned packages and its resources.
It has no network unless you allow it under **Edit**, and even then never Lake, the workspace or the
cluster; it holds no credentials; its data is handed to it, and its result taken from it, only by
Lake. Each run's CPU, memory and GPU time is metered on **Usage**.

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

When Auto-Suggest is not running on this Lake, the page says *a new scan is not available*;
earlier Opportunities stay listed. **Design → Agents** shows which of Orama's agents run
([Agents](/docs/agents)).

## Permissions

| To | You need, on the account that owns the workspace |
|---|---|
| Browse solutions, methodologies and opportunities | `read` |
| Create, edit, duplicate or delete your own solutions, or start one as a project | `write` |
| Create, edit, validate, train, duplicate or delete your own methodologies | `write` |
| Start or confirm a solution, run it, change its policy, approve A1 decisions, record outcomes | `write` |
| Scan, create project drafts, dismiss | `write` |

Only the person who started a solution (or the operator) can confirm its foundation. You hold
`admin` on your own workspaces; in an organization, members read unless they were granted `write`.

## Limits

- Delivery is a webhook, Salesforce or HubSpot; its endpoint and owner are required.
- Only one of Orama's solutions that runs on this engine, or one in preview, can be started with
  **Start this solution**; the others are listed with where their work is tracked.
- Orama's solutions and methodologies cannot be edited or deleted; duplicate one to make it yours.
- A methodology of your own runs only once Validate passed for its current code. Its training data
  is at most 1,000,000 rows and what it learns at most 24 MiB. Serving its model from Ship → Models is
  not available yet. Experiments over its parameters run in **Train → Experiments**.
- Your own solution starts as a project with its first pipeline, not with the governed foundation
  (policy, holdout, ROI readout) Orama's get.
- A name is at most 200 bytes, the problem and decision 4,000 bytes each, and a solution composes at
  most 32 methodologies.
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
| `forbidden` when you edit or delete a solution | It is one of Orama's, which are read-only. Open it and press **Duplicate to edit**. |
| `invalid_input` at `/measurement/metric`, `/problem` or `/decision` | A solution of your own needs its problem, its decision and the metric its return is measured by. |
| `invalid_input` at `/methodologies/...` | Write methodology ids such as `M-1`, each once, with a level of 1 to 4 after a colon. |
| `forbidden` when you edit or delete a methodology | It is one of Orama's. Press **Duplicate to edit**. |
| `conflict`: *validate this methodology first* | Its current code has not passed Validate. Press **Validate**. |
| Validate lists `entrypoint.score`, `packages` or `manifest.…` | The file it names is missing that part; fix it and Commit, then Validate again. |
| `unauthorized` | You have `read` but not `write`, or you did not start this solution. |

## Related Pages

- [AI Projects and Pipelines](/docs/design)
- [Datasets and Loading Data](/docs/load-data)
- [Agents](/docs/agents): the Agent Library
- [Train a Model](/docs/train)
- [Ask Orama](/docs/ask-orama)
