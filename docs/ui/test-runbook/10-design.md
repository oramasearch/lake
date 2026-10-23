# Design

The Design group: Projects, Project, Pipeline, Opportunities (also at `/design/discover`),
Solutions, Solution, Methodologies, Methodology, Agents, Agent, the Agent Factory, the old Agent
Library address and Catalogue Icons. It also makes `TD-PROJECT`, `TD-MODEL` and `TD-AGENT`, which
later files use. Source: `frontend/src/pages/design/`, `pages/train/FactoryPage.tsx`,
`pages/train/agents.tsx`.

## Before you start

- Personas: `owner`, `member`, `writer`, `newcomer`, `visitor`, `operator`.
- Test data: `TD-DATASET` in `W1`, `TD-W2` (left empty of projects and Opportunities).
- Extra data this file needs:
  - `TD-EVENTS`: `events.jsonl` from `TD-CSV-ODD` loaded into `W1` as `qa_<MMDD>_events` through
    **Data → Datasets → Load data**. It has no `churned` column, so a run trained on it should fail.
  - The Agent Library on the environment: note which of `agent-autosuggest`, `agent-ask-orama`,
    `agent-project` and `agent-creator` are **Available** on **Design → Agents** (filter **By Orama**).
    Cases that need an agent that is not running are `blocked`.
  - The engine's methodology list: on **Design → Methodologies**, note one Orama methodology that
    shows "Runs today" (usually `M-1`) and one that shows "Not yet".
  - On **Design → Solutions**, note one Orama solution that shows "Runs today" (or is in preview)
    and one that shows "Not yet".
- Names made here: projects `qa-<MMDD>-retention` (this is `TD-PROJECT`) and `qa-<MMDD>-scratch`;
  solution `qa-<MMDD>-late-payments`; methodology `qa-<MMDD>-late-payers`; agents
  `qa-<MMDD>-native-agent` and `qa-<MMDD>-frontier-agent` (together `TD-AGENT`).
- Workspace pages are at `/w/<workspace id><path>`. "W1" below means the `owner`'s first workspace.
- Every Design write goes to `POST /w/<id>/v1/workspace_act` or a named capability; engine reads go to
  `POST /w/<id>/v1/workspace_query`. Watch the request body's `capability` in the Network panel when a
  case names one.

## Workspace states on Design pages

#### DSGN-001 · Design pages ask a newcomer to start a workspace · P1
As: newcomer

1. Open **Design → Projects**, then **Solutions**, **Methodologies** and **Opportunities** from the rail.

Expect:
- Each page shows "Start Your Workspace" with "Design, Train and Ship run in a workspace of your own,
  so your projects, data and models are never shared with another customer. It takes a minute or two
  to prepare." and **Start your workspace**.
- No header button shows: no **New project**, **New solution**, **New methodology**.
- Do not press **Start your workspace** here; [04-workspaces.md](04-workspaces.md) covers it.

#### DSGN-002 · Orama's agents show under the gate · P2
As: newcomer

1. Open **Design → Agents**.

Expect:
- The gate shows, and below it a section "By Orama" with "Agents Orama provides, each for one job.
  Start your workspace to design your own or duplicate one of these." and a card per library agent.
- No **New agent** and no **Describe it to the Agent Factory** in the header.
- Each card shows the badge "By Orama" and one of "Available", "Offline tool", "Not running on this Lake".

#### DSGN-003 · The operator has no workspace on Design pages · P2
As: operator

1. Open **Design → Projects** and **Design → Agents** from the rail.

Expect:
- Each page shows "No Workspace Yet" with "You have no workspace yet." and **Workspaces**.

#### DSGN-004 · An address without a workspace opens one · P2
As: owner

1. Open `/design/projects` (no `/w/` prefix), then `/design/solutions?x=1`.
2. Open `/w/<W1 id>` and `/w/<W1 id>/`.

Expect:
- Step 1 lands on `/w/<remembered workspace id>/design/projects`, then `/w/<id>/design/solutions?x=1`
  with the query kept.
- Step 2 lands on `/w/<W1 id>/design/projects`.

## Projects

#### DSGN-005 · An empty workspace's Projects page · P1
As: owner · Needs: TD-W2

1. Pick `TD-W2` in the workspace picker and open **Design → Projects**.

Expect:
- "No Projects Yet" with "A project starts from the decision it should improve. Or let the
  Auto-Suggest Agent recommend one from your data." and the buttons **New project** and **See your
  Opportunities**.
- The header shows **New project**.
- **See your Opportunities** opens `/w/<TD-W2 id>/design/opportunities`.

#### DSGN-006 · New Project refuses a bad name · P2
As: owner

1. In `W1`, open **Design → Projects** and press **New project**.
2. Check the drawer, then type `Retention` in **Name** and press **Create project**.
3. Change it to `-retention` and press **Create project**.

Expect:
- The drawer is titled "New Project" with "Start from the decision it should improve, then build its
  pipelines on the project page."
- Fields: **Name** with " *", help "Lowercase letters, digits, - and _." and placeholder
  `retention-review`; **Description** (placeholder "Which enterprise accounts will not renew?");
  **The decision it should improve** (placeholder "Which accounts the team calls first each week,
  measured by accounts saved per quarter").
- Steps 2 and 3: the browser refuses the name (pattern mismatch) and no `create_project` request is sent.
- An empty **Name** is refused by the browser too.

#### DSGN-007 · Create TD-PROJECT · P0
As: owner · Needs: TD-DATASET

1. In `W1`, on **Design → Projects**, press **New project**.
2. **Name** `qa-<MMDD>-retention`, **Description** `Keep paying accounts`, **The decision it should
   improve** `Which accounts customer success calls first each week`.
3. Press **Create project**.

Expect:
- A `workspace_act` request with `capability: "create_project"` and the three fields.
- The project page opens at `/w/<W1 id>/design/project?name=qa-<MMDD>-retention`.
- The title is `qa-<MMDD>-retention` with the chip "no pipelines"; the eyebrow is "Design · Project".
- The **Decision** card ("What It Should Improve") shows the decision, and below it "Keep paying accounts".

#### DSGN-008 · A project name is unique in the workspace · P1
As: owner · Needs: DSGN-007

1. Press **New project**, type **Name** `qa-<MMDD>-retention` and press **Create project**.

Expect:
- The message from Lake (`conflict`, "a project named … already exists in this workspace") shows under
  **Name** with the field marked invalid. The drawer stays open; no second project appears.

#### DSGN-009 · Create the scratch project with no description · P1
As: owner

1. Press **New project**, **Name** `qa-<MMDD>-scratch`, leave the other fields empty, press **Create project**.
2. Open **Design → Projects**.

Expect:
- The `create_project` input has only `name` (an empty description and decision are left out).
- On the project page the **Decision** card says "No decision recorded." and the **Dataset** card says
  "Chosen when you build or run a pipeline."
- In the list, the row's second line says "No decision recorded".

## Project and pipeline: building TD-PROJECT

#### DSGN-010 · The project page before any pipeline · P0
As: owner · Needs: DSGN-007

1. Open `qa-<MMDD>-retention` from **Design → Projects**.

Expect:
- Tabs **Overview**, **Pipelines** with count 0, **Runs**; no **Solution** tab.
- **Overview** shows the panel "Plan" / "Overview" with "Keep paying accounts" and "No pipelines yet.
  Describe what the project should predict or answer, and Lake builds one."
- **Latest Runs** says "No runs yet."
- The side column: **Status** (chip "no pipelines"; **Pipelines** 0, **Approved** 0, **Latest run**
  "none yet"), **Decision**, **Dataset**, **Actions** with **Opportunities** and **Delete project**.
- The header shows **Build pipeline**.

#### DSGN-011 · Build a pipeline on TD-DATASET · P0
As: owner · Needs: DSGN-007, TD-DATASET

1. Press **Build pipeline**.
2. In **What should it predict or answer?** type `Predict which accounts will churn (the churned
   column), with the reasons`.
3. In **From data** choose `qa_<MMDD>_accounts`.
4. In **Predicts** choose `churned`; in **Each row is about** choose `account_id`.
5. Press **Build pipeline**.

Expect:
- The drawer is titled "Build a Pipeline" with "Describe what it should predict or answer; Lake builds
  the steps from a template you can edit."
- While typing step 2, **Pipeline name** fills with "Accounts churn" and **Model name** with
  `accounts_churn_model`.
- **From data** lists datasets by name; the first option is "Let the builder describe its sources";
  no table whose name ends `_results` is offered.
- **Predicts** and **Each row is about** appear only after a dataset is chosen; each starts at "Not chosen yet".
- After step 5: a `compile_pipeline` request with `project`, `intent`, `name: "Accounts churn"`,
  `dataset`, `label_column: "churned"`, `entity_column: "account_id"`.
- The pipeline page opens at `…/design/pipeline?project=qa-<MMDD>-retention&graph=Accounts%20churn&dataset=…#run`,
  scrolled to "Approve, Check, Run and Deploy", with **Train on** set to the dataset.
- When Lake names a template, a toast says "Built from <template>, its steps named from <dataset>."

#### DSGN-012 · Approve the pipeline · P0
As: owner · Needs: DSGN-011

1. On the pipeline page, read the "Approve, Check, Run and Deploy" panel.
2. Press **Approve**.

Expect:
- Before step 2: the title chip says "proposed"; the Pipeline card's **Approval** says "proposed" and
  "approve before it runs"; the Check cell says "Approve the pipeline first; checks run on an approved
  pipeline."; **Check**, **Check and run** and **Deploy** are disabled.
- After step 2: a `workspace_act` with `capability: "approve_plan"`; the toast "Pipeline approved".
- The title chip and **Approval** say "approved" with the time in UTC; the approve cell says
  "Approved <time> UTC. Lake verifies and runs only an approved pipeline." beside a disabled
  **Approved**; the cell keeps its place (the layout does not jump).
- **Check**, **Check and run** and **Deploy** are enabled; the Check cell says "Run Lake's correctness
  checks without running anything."

#### DSGN-013 · Check the pipeline, and list the checks that did not apply · P1
As: owner · Needs: DSGN-012

1. Press **Check**.
2. If a line "… checks did not apply to this pipeline, so they are left out." shows, press **List them
   for an audit**.

Expect:
- A `workspace_act` with `capability: "gate"`; the page scrolls to the top.
- A panel "Checks" with a verdict chip (`pass`, `warn` or `fail`), "It may run and deploy." (or "It
  cannot run or deploy until the failing checks pass." in red), and a table **Check**, **Status**, **Why**.
- Step 2 sends `include_not_applicable: true`; the table then has rows with status "did not apply", and
  the line ends "; they are listed above as did not apply."

#### DSGN-014 · Check and run: the first run succeeds · P0
As: owner · Needs: DSGN-012

1. In "Approve, Check, Run and Deploy", keep **Train on** at `qa_<MMDD>_accounts`, **Hold out for
   measuring** at `0.2` and **Model name** at `accounts_churn_model`.
2. Press **Check and run**.
3. Wait on the run's page until it finishes.

Expect:
- A `run_pipeline` request with `dataset_path`, `model_name: "accounts_churn_model"` and
  `holdout_fraction: 0.2`.
- The run's page opens (`/w/<W1 id>/train/run?id=<run id>`) and ends `succeeded`.
- A failing gate instead keeps the pipeline page and shows the "Checks" panel; record that as a failure
  of this case.

#### DSGN-015 · A second run adds a version to the same model (TD-MODEL) · P1
As: owner · Needs: DSGN-014

1. Open the pipeline again from the project's **Pipelines** tab.
2. Check that **Model name** shows `accounts_churn_model`, then press **Check and run**.

Expect:
- **Model name** is prefilled with the model this pipeline already trained.
- The run succeeds. **Train → Models** lists `accounts_churn_model` with two versions. This is `TD-MODEL`.

#### DSGN-016 · A run on data without the label fails (TD-PROJECT's failed run) · P1
As: owner · Needs: DSGN-015, TD-EVENTS

1. On the pipeline page, set **Train on** to `qa_<MMDD>_events` and press **Check and run**.

Expect:
- Either the "Checks" panel shows a failing check, or the run's page opens and the run ends `failed`.
  For `TD-PROJECT` a failed run is needed: if the gate blocked it, the run is not recorded; note it in
  the results and use another failing run (for example a deleted column) if later files need one.
- The pipeline's **Runs** panel lists the runs newest first, with **Run**, **Status**, **Started**,
  **Outputs**; the failed one has a red `failed` chip.

#### DSGN-017 · The project page after its runs · P0
As: owner · Needs: DSGN-016

1. Open `qa-<MMDD>-retention`.
2. Open each tab: **Overview**, **Pipelines**, **Runs**.

Expect:
- The title chip and **Status** say "approved"; **Pipelines** 1, **Approved** 1; **Latest run** shows the
  newest run's status chip and a relative time.
- **Overview** says "1 of 1 pipelines approved." and lists **Latest Runs**.
- **Pipelines** shows the row "Accounts churn" (its id below in mono), a green methodology chip (or
  "Runs"), approval `approved` and **Check and run**.
- **Runs** lists at most 5 runs: **Run**, **Status**, **Stage Reached**, **Started**, **Error**; a failed
  run's error is in red. Each run id opens its run page.
- The **Dataset** card shows the latest run's dataset.
- The address carries `tab=pipelines` and `tab=runs`; **Overview** has no `tab=`.

#### DSGN-018 · Deploy the approved pipeline · P1
As: owner · Needs: DSGN-012

1. On the pipeline page press **Deploy**.

Expect:
- A `workspace_act` with `capability: "deploy"` and `target: "local"`; the page scrolls up.
- A green banner "Deployed" with "Deployment `<id>` to `<target>`." and a panel "Its Checks".

## Project page: other ways to build

#### DSGN-019 · Build a pipeline without a dataset · P1
As: owner · Needs: DSGN-009

1. Open `qa-<MMDD>-scratch`, press **Build pipeline**.
2. Type `Predict which accounts will churn (the churned column), with the reasons`, leave **From data**
   at "Let the builder describe its sources", press **Build pipeline**.
3. Press **Build pipeline** again and type the same description.

Expect:
- After step 2 the drawer closes, a toast says "Pipeline Accounts churn built" (with " from <template>;
  rename its steps to fit your data" when Lake names one), and the **Pipelines** count is 1.
- In step 3, **Pipeline name** shows "Accounts churn (2)". Press **Cancel**.

#### DSGN-020 · Approve and run from the project's Pipelines row · P1
As: owner · Needs: DSGN-019

1. On `qa-<MMDD>-scratch`, tab **Pipelines**, press **Approve** on the row.
2. Press **Check and run** on the same row.

Expect:
- Step 1: toast "Pipeline approved"; the row's approval is `approved` and its button is **Check and run**.
- Step 2: when the gate passes, the browser opens **Train → Runs** (the list), not the run's page. See
  suspected defect 1.
- When the gate fails, under the button: "The pipeline did not pass its checks: <verdict>".

#### DSGN-021 · A typed model name opens the run form · P2
As: owner · Needs: DSGN-009

1. On `qa-<MMDD>-scratch`, press **Build pipeline**, type `Score accounts by monthly spend`, type
   **Model name** `qa_<MMDD>_spend_model`, press **Build pipeline**.

Expect:
- The pipeline page opens with `model=qa_<MMDD>_spend_model` in the address and `#run`; **Model name**
  in the run form shows `qa_<MMDD>_spend_model`.

#### DSGN-022 · Choose a methodology; a pipeline that trains nothing says so · P2
As: owner · Needs: DSGN-009

1. Press **Build pipeline** on `qa-<MMDD>-scratch`.
2. Open **Methodology**.
3. Close the drawer. On the **Pipelines** tab read the **Methodology** column.

Expect:
- **Methodology** shows only when the engine lists runnable methodologies: first "Let Lake choose from
  the description", then entries as "<name> (<id>)", help "Only the methodologies this workspace's
  engine runs are listed." Only methodologies marked "Runs today" on **Design → Methodologies** are listed.
- In the table, a runnable pipeline has a green chip with its methodology id (or "Runs"); one that does
  not train has an amber "Trains nothing yet" or "Trains nothing yet: needs M-n"; "—" when the engine
  says neither.

#### DSGN-023 · Start an empty pipeline · P1
As: owner · Needs: DSGN-009

1. On `qa-<MMDD>-scratch`, tab **Pipelines**, find "Other Ways to Start a Pipeline".
2. Press **Create** with **Name** empty.
3. Type **Name** `qa-<MMDD>-by-hand` and press **Create**.

Expect:
- **Start from** offers **Empty**, **From n8n**, **From Python**; **Empty** is pressed (`aria-pressed`).
- Step 2: the browser refuses the empty name.
- Step 3: `create_pipeline` with `from: "empty"`; the new pipeline's page opens.

#### DSGN-024 · Import from n8n and build from Python · P2
As: owner · Needs: DSGN-009

1. Choose **From n8n**. Type `not json` in **n8n workflow export (JSON)** and press **Import from n8n**.
2. Choose **From Python**, check the field, then paste a short Python pipeline and press **Build from Python**.

Expect:
- Switching mode clears the fields.
- Step 1: Lake's refusal shows above the fields (role `alert`); nothing is created.
- **From Python** has **Name** (placeholder "python program") and **Python pipeline** with help "Lake
  reads it and lowers it to a pipeline; the code never runs."
- Step 2 opens the new pipeline, or shows Lake's refusal for code it cannot lower.

#### DSGN-025 · Train a model on this, from a dataset · P1
As: owner · Needs: TD-DATASET

1. Open the dataset `qa_<MMDD>_accounts` (**Data → Datasets**) and press **Train a model on this**.
2. Read the drawer, then press **Cancel**.
3. Repeat step 1, change **Name** to `qa-<MMDD>-from-data` and press **Create project**.

Expect:
- Projects opens with `?new=1&dataset=<database>.qa_<MMDD>_accounts` and the "New Project" drawer open.
- Subtitle: "Start from the decision it should improve. Creating it opens Build pipeline with
  <dataset> chosen as its data."
- **Name** is prefilled `qa_<MMDD>_accounts-model`; **Description** "A model trained on <dataset>".
- Step 2: the address loses `new` and `dataset`; reloading does not reopen the drawer.
- Step 3: the project opens with "Build a Pipeline" open and **From data** set to the dataset; closing
  it removes `build` and `dataset` from the address.

#### DSGN-026 · A project address without a name, or with an unknown one · P2
As: owner

1. Open `/w/<W1 id>/design/project`.
2. Open `/w/<W1 id>/design/project?name=qa-<MMDD>-nothing`.

Expect:
- Step 1: "No Project Named" with "Open a project from the Projects list." and **All projects**. The
  title is "Project" and no **Build pipeline** shows.
- Step 2: an error block "Couldn't load this project" with Lake's message.

## Pipeline page

#### DSGN-027 · An empty pipeline's page · P1
As: owner · Needs: DSGN-023

1. Open `qa-<MMDD>-by-hand` from `qa-<MMDD>-scratch`'s **Pipelines** tab.

Expect:
- Title `qa-<MMDD>-by-hand` with a status chip; eyebrow "Design · Pipeline of qa-<MMDD>-scratch"; sub
  "The steps Lake runs and how data flows between them. A changed pipeline is approved again before it runs."
- Breadcrumb Design › Projects › `qa-<MMDD>-scratch` › `qa-<MMDD>-by-hand`.
- The "Graph" panel's label says "<n> steps · <m> edges", footer "Solid arrows carry data; dashed arrows
  only order steps. Select a step to inspect or change it."; with no steps it says "No Steps Yet" and
  "Add the first one under Change the Pipeline."
- The side "Step" panel says "Select a Step" and "Select a step in the graph to inspect or change it."
- The "Pipeline" panel lists **Project** (a link), **Approval**, **Steps**, **Edges**, **Latest run** "none yet".
- When the engine says it cannot train, a banner "This Pipeline Trains Nothing Yet" says "It has no
  model step the engine can run, so a run only passes through its steps." (or "It needs <M-n>, which
  this workspace's engine does not run yet.") and "Build a pipeline with a methodology that runs, or ask
  the Project Agent to change it."
- **Runs** says "No runs of this pipeline yet."

#### DSGN-028 · Add steps, an edge, and rename the pipeline · P1
As: owner · Needs: DSGN-027

1. Under "Change the Pipeline", **Kind** `connect`, **Name** `Load accounts`, press **Add step**.
2. **Kind** `model`, **Name** `Score accounts`, press **Add step**.
3. **From** `Load accounts (…)`, **To** `Score accounts (…)`, **Carrying** `data_flow`, press **Add edge**.
4. In **Name** under it, type `qa-<MMDD>-by-hand-2` and press **Rename pipeline**.

Expect:
- **Kind** offers `connect`, `clean`, `model`, `index`, `serve`, `act`; **Carrying** offers `data_flow`
  and `control`.
- Each step sends one `edit_pipeline` (`op` `add_step`, `add_edge`, `rename`) and toasts "Pipeline saved".
- After each **Add step**, the new step is selected (`step=` in the address) and the graph shows it.
- After step 3 the "Edges" table lists **From**, **To**, **Carrying** with **Remove**.
- After step 4 the address's `graph=` and the title are `qa-<MMDD>-by-hand-2`.

#### DSGN-029 · Select a step and change it · P1
As: owner · Needs: DSGN-028

1. Press the `Score accounts` box in the graph.
2. Rename it to `Score each account` with **Rename step**.
3. Set **Kind** to `act` with **Change kind**.
4. In the step's **From** row press the `Load accounts` step id.

Expect:
- Step 1 does not reload the page; `step=<id>` appears in the address. Ctrl- or Cmd-click opens the
  link in a new tab instead.
- The panel's label is "Step · model", title `Score accounts`, the id in mono; rows **Kind**, the step's
  attributes, **Last run** ("not in the last run"), **From** (the connect step and `data_flow`), **To**
  ("nothing").
- Steps 2 and 3 each toast "Pipeline saved" and the panel updates.
- Step 4 selects `Load accounts` without a reload.
- **Back** returns to the previous selection.

#### DSGN-030 · Connect, remove an edge and remove a step · P1
As: owner · Needs: DSGN-029

1. With `Load accounts` selected, choose **Then** `Score each account (…)`, **Carrying** `control`,
   press **Connect**.
2. In the "Edges" table, press **Remove** on one edge.
3. Select `Score each account` and press **Remove step**.

Expect:
- Step 1 sends `op: "add_edge"`; a duplicate edge is refused with Lake's message above the form.
- Step 2 sends `op: "remove_edge"`; the row goes.
- Step 3 says "Removing a step removes its edges too." beside **Remove step**; pressing it removes the
  step without a confirmation, clears `step=` and toasts "Pipeline saved".

#### DSGN-031 · An edit that would make a cycle is refused · P2
As: owner · Needs: DSGN-028

1. Add a second step and edges A → B and B → A with **Add edge**.

Expect:
- The second edge is refused with Lake's `invalid_input` message (a cycle) above the **Add edge** form;
  the graph is unchanged.

#### DSGN-032 · Editing an approved pipeline makes it proposed · P1
As: owner · Needs: DSGN-020

1. Open the approved "Accounts churn" pipeline of `qa-<MMDD>-scratch`.
2. Add a step `clean` named `Drop test accounts`.

Expect:
- After the toast "Pipeline saved", the title chip and **Approval** say "proposed" with "approve before
  it runs".
- **Check**, **Check and run** and **Deploy** are disabled; the Check cell says "Approve the pipeline
  first; checks run on an approved pipeline."

#### DSGN-033 · A refusal before approval reads in the page's words, and Approve clears it · P2
As: owner · Needs: DSGN-032

1. Click into **Model name** in the run form and press **Enter**.
2. Press **Approve**.

Expect:
- Step 1 sends `run_pipeline` although the button is disabled (suspected defect 3). The message under
  the form is "Approve the pipeline first; checks run on an approved pipeline." and never names
  `approve_plan`.
- Step 2: toast "Pipeline approved"; the refusal from step 1 is gone.

#### DSGN-034 · Hold-out bounds · P2
As: owner · Needs: DSGN-012

1. On `TD-PROJECT`'s pipeline, set **Hold out for measuring** to `1` and press **Check and run**.

Expect:
- The browser refuses the value (maximum 0.9); no request is sent. The help says "The share of rows kept
  out of training to measure the model; 0 measures nothing."

#### DSGN-035 · Ask the Project Agent and apply its proposal · P1
As: owner · Needs: DSGN-028; agent-project Available

1. On `qa-<MMDD>-by-hand-2`, under "Ask the Project Agent", type `Train on qa_<MMDD>_accounts only and
   hold out 20% of rows` and press **Propose a change**.
2. Wait for the proposal.
3. Press **Apply**.

Expect:
- The panel's label is "It proposes edits for you to review" with "It reads your data and proposes edits
  for you to review; nothing changes until you apply them."
- Step 1: an `agent_turn` with `agent: "agent-project"`, `kind: "pipeline"`; the address gets `task=` and
  `#proposal`; a banner "The Project Agent is working on it…" with "It reads your data and prepares the
  edits; nothing changes until you apply them. This page checks again every 3 seconds."
- Step 2: a panel labelled "Proposed by the Project Agent" with its summary as title, "Nothing has changed
  yet. Applying saves these edits; the pipeline then needs approval again before it runs.", the
  **Edits** list, **Run settings** ("Train on `…`", "Hold out 20% of rows for measuring") and **After**
  (a drawn graph), with **Apply** and **Discard**.
- Step 3: `edit_pipeline` with the proposal's `edits`; the page opens at `#run` with **Train on** and
  **Hold out for measuring** set from the proposal; the pipeline is "proposed".

#### DSGN-036 · Discard a proposal; answer a question · P2
As: owner · Needs: DSGN-035

1. Ask the Project Agent `make it better` and wait.
2. If it asks a question, type a **Reply** and press **Reply to the Project Agent**.
3. On a proposal, press **Discard**.

Expect:
- A question shows as "Project Agent Question" with the question in Markdown and **Reply** (required).
  The reply sends `agent_turn` with the same `conversation_id`, and `task=` changes to the new task.
- A turn with nothing to apply shows "The Project Agent did not propose a change" with one plain
  sentence and "Ask it again with more detail, or change the steps by hand."
- A proposal that cannot be applied lists "Why it cannot be applied" and shows no **Apply**.
- **Discard** removes `task=` and the panel; the graph is unchanged.

#### DSGN-037 · An unreadable pipeline address · P2
As: owner

1. Open `/w/<W1 id>/design/pipeline?project=qa-<MMDD>-scratch&graph=qa-<MMDD>-nothing`.

Expect:
- An error block "Couldn't load this pipeline" with Lake's message, or "Pipeline Not Readable" with "This
  pipeline could not be read from the workspace." and **Back to the project**.

## Opportunities

#### DSGN-038 · Opportunities when the Auto-Suggest Agent does not run · P2
As: owner · Needs: a Lake where agent-autosuggest is not Available (else `blocked`)

1. Open **Design → Opportunities**.

Expect:
- A grey banner "The Auto-Suggest Agent is not running on this Lake, so a new scan is not available."
- No **Scan my workspace** anywhere; Opportunities from earlier scans are still listed.

#### DSGN-039 · An empty workspace's Opportunities · P1
As: owner · Needs: TD-W2; agent-autosuggest Available

1. In `TD-W2`, open **Design → Opportunities**.

Expect:
- Title "Opportunities", sub "What your data could be used for. The Auto-Suggest Agent reads the data
  your workspace can reach and recommends what to build from Orama's solutions catalogue; it never
  changes your data."
- A banner "Scan your workspace" with "The Auto-Suggest Agent reads your workspace's datasets and their
  columns and recommends what to build. It runs in the background and changes nothing." and **Scan my
  workspace**.
- "No Opportunities Yet" with "Scan your workspace to see what your data could be used for." and **Scan
  my workspace**. No figures.

#### DSGN-040 · Scan W1 · P1
As: owner · Needs: TD-DATASET; agent-autosuggest Available

1. In `W1`, open **Design → Opportunities** and press **Scan my workspace**.
2. Leave the page for **Design → Projects**, then come back with **Back**.
3. Wait for the scan to finish.

Expect:
- Step 1: an `agent_turn` with `agent: "agent-autosuggest"`, `kind: "discover"`, `message: "What could
  this workspace build from its data?"`; the address gets `?task=<id>`.
- While it works: "Scanning your workspace…" with "The Auto-Suggest Agent is reading your datasets and
  matching them to the solutions catalogue. You can leave this page; the Opportunities are kept when it
  finishes. This page checks again every 3 seconds."
- Step 2 still shows the scan (the `task=` is kept).
- When done: figures **Recommended** (sub "from the latest scan"), **Made into projects**, **Not yet**
  (sub "the data does not support yet"), **Dismissed**; the banner says "Last scanned <relative time>",
  "Scan again after your data changes; earlier Opportunities stay." and **Scan again**.
- A failed scan shows "The scan did not finish" with its message.

#### DSGN-041 · What an Opportunity card shows · P1
As: owner · Needs: DSGN-040

1. Read the cards under "Recommended" and the "Not Yet" panel.

Expect:
- Each card has a checkbox and the intent as title; a 32 px icon; a label like "Catalogue solution S-1 ·
  M-1"; a chip "Runs today: <name> (M-n)" (green) or "Needs M-n, not supported yet (<issue>)" (amber);
  the reason in Markdown and "**What it changes:** …"; **Dismiss** and **Create project**.
- "Not Yet" (label "What your data does not support yet, and why") lists "<intent>: <why>".
- "The latest scan recommended nothing new." shows when the newest scan has no new card.

#### DSGN-042 · Create a project from one Opportunity · P1
As: owner · Needs: DSGN-041; agent-project Available

1. On a card with "Runs today", press **Create project**.
2. Press **Cancel**, then **Create project** again and **Create project draft**.
3. Open the new project from **Design → Projects**.

Expect:
- A dialog "Create a project draft?" with "A project draft is made from “<intent>”. Nothing runs until
  you approve a plan." and **Cancel**, **Create project draft**.
- Step 2 sends `create_projects_from_opportunities` with that one id and opens **Design → Projects**.
- The project page shows "The Project Agent is designing this project's pipeline…" with "It reads your
  data and the Opportunity this project came from; the pipeline appears here when it is done. You can
  leave this page. This page checks again every 3 seconds."; **Status** lists **Project Agent** with the
  task status; the footer says "This page checks again every 3 seconds while work is under way."
- When it finishes the pipeline appears under **Pipelines**, "proposed".
- If the design fails: "The Project Agent could not design this project's pipeline" with "<message>
  Describe it instead." and **Describe it**, which opens "Build a Pipeline".
- Back on Opportunities, the card is under "Made into Projects" with a link "<project> →".

#### DSGN-043 · Create project drafts in bulk · P1
As: owner · Needs: DSGN-041

1. Press **Create project drafts** with nothing ticked.
2. Tick the confirmation "Create a project draft for each selected Opportunity. Nothing runs until you
   approve a plan." and press **Create project drafts** again.
3. Tick two cards.
4. Untick the confirmation and press the button.
5. Tick the confirmation and press **Create project drafts (2)**.

Expect:
- Step 1: "Confirm to create the drafts."
- Step 2: "Choose at least one Opportunity."
- Step 3: the button reads "Create project drafts (2)"; ticked cards have a blue edge.
- Step 4: "Confirm to create the drafts."
- Step 5: one request with both ids; **Design → Projects** opens with two new projects.
- An Opportunity whose methodology does not run makes a project with no pipeline; the page says nothing
  about it (suspected defect 17).

#### DSGN-044 · Dismiss an Opportunity · P1
As: owner · Needs: DSGN-041

1. Press **Dismiss** on a card.
2. Open "Dismissed (<n>)".

Expect:
- `dismiss_opportunity` with its id; toast "Opportunity dismissed"; the card leaves "Recommended".
- The **Dismissed** figure goes up by one; the disclosure lists its intent.

#### DSGN-045 · A second scan folds earlier ones · P2
As: owner · Needs: DSGN-040

1. Press **Scan again** and wait.

Expect:
- "Recommended" shows the newest scan's cards. Unacted cards from earlier scans are under "Earlier scans
  (<n>)" with "What earlier scans recommended and nobody has acted on. The latest scan above replaces
  them; you can still pick or dismiss one."
- The **Recommended** figure's sub says "<n> from earlier scans".

#### DSGN-046 · The Discover address and workspace isolation · P2
As: owner · Needs: DSGN-040, TD-W2

1. Open `/w/<W1 id>/design/discover`.
2. Open **Design → Opportunities** in `TD-W2`.

Expect:
- Step 1 shows the same Opportunities page; the rail marks **Opportunities**.
- Step 2 shows none of `W1`'s Opportunities.

## Solutions

#### DSGN-047 · The Solutions catalogue · P0
As: owner

1. In `W1`, open **Design → Solutions**.

Expect:
- Title "Solutions", sub "Each solution answers one business problem end to end. Start Orama's, or write
  your own, and it becomes a project in your workspace."; header **New solution**.
- Figures **Orama's** ("in the catalogue, read-only"), **Yours** ("written in this workspace"), **Run
  here** ("of Orama's, on this workspace's engine").
- Sections in order: "Yours", "Orama's Modules", "Orama's, in Design" (only when there are sketches).
- With no solution of your own, "Yours" shows "No Solutions of Your Own Yet" with "Write one for a
  problem the catalogue does not cover, or open one of Orama's and press Duplicate to edit." and **New solution**.
- Each Orama card: a 32 px icon, the id as label, the name, the badge "Orama" (tooltip "Orama's catalogue:
  read-only"), a run badge ("Runs today", "Runs, in part", "Not yet" or "In design"), the promise and
  methodology chips (`M-3`, with `L2+` for a minimum level above 1). The whole card opens the solution.
- When the engine does not list the catalogue: a grey banner "Orama's Solutions Are Not Listed" ending
  "Your own solutions are listed below."

#### DSGN-048 · Filter and search the solutions · P2
As: owner

1. Type `churn` in "Search solutions…".
2. Clear it. Press **Orama**, then a state chip such as **Runs today**.
3. Press **Yours**.
4. Type `zzzz`.

Expect:
- Step 1 keeps cards whose id, name, promise, problem, use case, decision or notes contain "churn".
- Step 2: the state chips start with **Any state**; only "Runs today" Orama cards stay; "Yours" is hidden.
- Step 3: the state chips disappear.
- Step 4: "No Solution Matches" with "Clear the search or a filter to see more."

#### DSGN-049 · New Solution refuses bad methodologies · P2
As: owner

1. Press **New solution**. Fill **Name** `qa-<MMDD>-late-payments`, **The business problem**, **The
   decision it drives**, **Metric** `Days sales outstanding`.
2. **Methodologies** `M 1`, press **Create solution**.
3. `M-1:5`, then `M-1, M-1`, each with **Create solution**.

Expect:
- The drawer "New Solution" ("The business problem, the decision it drives and how its return is
  measured.") has **Name**, **The business problem**, **The decision it drives**, the heading "How Its
  Return Is Measured", **Metric**, **Baseline** ("Where it stands today."), **Target** ("Where it should
  go."), the heading "What It Composes", **Methodologies** (help "Ids separated by commas, with the level
  each starts at after a colon when above 1: `M-1, M-3:2`." and "This workspace's engine knows …"),
  **Pipeline template or reference**, **Notes**. Name, problem, decision and metric are required.
- Step 2: "\"M 1\" is not a methodology id such as M-1 or M-3:2." above the form.
- Step 3: "M-1's minimum level must be 1 to 4.", then "M-1 is listed twice." No request is sent for any of them.

#### DSGN-050 · Create a solution of your own · P1
As: owner · Needs: DSGN-049

1. Set **Methodologies** to `M-1`, **Baseline** `52 days`, **Target** `40 days`, **Pipeline template or
   reference** `qa-<MMDD>-retention/Accounts churn`, press **Create solution**.

Expect:
- `create_solution`; the new solution opens with the badge "Yours" and eyebrow "Design · Your Solution".
- "What It Does": **The problem**, **The decision it drives**, **Methodologies** (an icon and a link `M-1`),
  **Pipeline** as a link to that pipeline.
- "How Its Return Is Measured": **Metric**, **Baseline** `52 days`, **Target** `40 days`.
- The side "Source" card: **From** "This workspace, editable", **Created** with a relative time.
- On **Design → Solutions** it is under "Yours" with its decision as text.

#### DSGN-051 · Edit your solution · P1
As: owner · Needs: DSGN-050

1. Press **Edit**, change **Target** to `38 days`, press **Save**.
2. Press **Edit**, replace **Metric** with a single space, press **Save**.

Expect:
- The drawer "Edit Solution" has the name as subtitle and the fields filled in.
- Step 1: `update_solution`; toast "Solution saved"; **Target** shows `38 days`; **Edited** appears in "Source".
- Step 2: Lake's `invalid_input` message shows under **Metric**.

#### DSGN-052 · An Orama solution is read-only · P1
As: owner

1. Open the Orama solution noted as "Runs today".
2. Open the one noted as "Not yet".

Expect:
- Eyebrow "Design · Solution S-n"; title with the badge "Orama" and its run badge; the promise as sub;
  a 64 px icon.
- Side: "Status" (**Id**, **From** "Orama's catalogue, read-only", **Catalogue**), "Make It Your Own"
  ("Orama's solutions are read-only. Duplicate this one to edit its problem, decision, measurement and
  methodologies in your workspace.").
- Main: "What It Does" (**The problem**, **Methodologies**, **How it acts**, **Decision speed**, **What data
  it needs**), "Levels" (numbered), **Runs**.
- The runnable one has **Start this solution** in the header and a "Start This Solution" panel with
  **Start**. The "Not yet" one has neither, and shows **Tracked in** with its issue.
- Neither has **Edit** or **Delete**.

#### DSGN-053 · Duplicate an Orama solution to edit · P1
As: owner

1. On the runnable Orama solution, press **Duplicate to edit**.

Expect:
- `duplicate_solution`; toast "Duplicated as <name>"; the copy opens with "Yours".
- "Source" shows **Duplicated from** "Orama's S-n" linking back; its icon is the original's.

#### DSGN-054 · Start your own solution as a project · P1
As: owner · Needs: DSGN-050

1. On `qa-<MMDD>-late-payments`, press **Start as a project**.
2. Keep **Project name** `qa-<MMDD>-late-payments`, **Methodology** `M-1`, no dataset; press **Start**.

Expect:
- The drawer "Start as a Project" says "Lake creates the project with this solution's problem and
  decision, then builds its first pipeline from the decision. A solution of your own does not get the
  governed foundation (policy, holdout and ROI readout) that Orama's solutions do."
- **Methodology** lists "Let Lake choose from the decision" and `M-1`; help "The methodology its first
  pipeline uses."
- `create_project` with `decision` "… Measured by Days sales outstanding, from 52 days to 38 days.", then
  `compile_pipeline`.
- Toast "Project qa-<MMDD>-late-payments created with pipeline <graph>"; the project opens.
- If the pipeline fails, the toast (red) says "Project qa-<MMDD>-late-payments created, but its first
  pipeline was not built: <message>" and the project still opens.

#### DSGN-055 · Start This Solution: the inputs · P2
As: owner · Needs: the runnable Orama solution

1. Press **Start this solution**, then **Start** with every field empty.
2. Fill all fields but type `abc` in **Value per outcome** (if the browser allows it), press **Start**.

Expect:
- The drawer "Start This Solution", subtitle "<id> <name>", has **Project name**, **Level**, **Dataset**
  ("Choose a dataset"), the heading "Where Actions Go", **Delivery** ("Choose delivery", **Webhook**,
  **Salesforce connector**, **HubSpot connector**), **Webhook URL or connector endpoint**, **Owner**,
  the heading "Economics", **Value per outcome**, **Investment**, **Currency** (help "These are your
  inputs. Lake does not supply defaults.").
- Step 1: the browser refuses the empty required fields.
- Step 2: "Level and economics must be valid numbers." if the value reaches the page; else the browser
  refuses it. See suspected defect 8 for a level select whose values are not numbers.

#### DSGN-056 · Start an Orama solution and confirm its foundation · P1
As: owner · Needs: the runnable Orama solution, TD-DATASET; agent-project Available

1. Fill **Project name** `qa-<MMDD>-sol`, a **Level**, **Dataset** `qa_<MMDD>_accounts`, **Delivery**
   **Webhook**, an HTTPS endpoint, **Owner** `qa`, **Value per outcome** `100`, **Investment** `1000`,
   **Currency** `USD`; press **Start**.
2. Wait for the proposal on the **Solution** tab.
3. Replace the JSON with `[]` and press **Confirm and create solution** with the box ticked.
4. Undo to the proposed JSON, tick "I reviewed this snapshot, these roles, predicates, delivery settings
   and economics.", press **Confirm and create solution**.

Expect:
- `start_solution`; the project opens on its **Solution** tab with "Foundation proposal: the Project
  Agent is reading the dataset…" and "This page checks again every 3 seconds."
- The proposal panel "Foundation Proposal", label "Review before anything is created", text "Review and
  edit the exact foundation sent to the engine. Delivery and economics are your inputs from Start.", and
  a **Foundation (JSON)** box.
- A dataset missing exclusion predicates shows "Actions will remain blocked." with "The dataset is
  missing these exclusion predicates: <names>. They are not treated as false."
- Step 3: "The foundation must be a JSON object."
- Step 4: `confirm_solution`; toast "Solution confirmed for qa-<MMDD>-sol".
- A failed proposal says "The Project Agent could not propose the foundation: <message>".

#### DSGN-057 · Operate the solution · P1
As: owner · Needs: DSGN-056

1. Read the **Solution** tab.
2. If **Run** shows, press it with **Delivery token (optional, used for this run only)** empty.

Expect:
- "Solution Stages" (its graph as label) with **Stage**, **Name**, **Kind**.
- With a missing predicate, instead of **Run**: "Run is blocked because an exclusion predicate is missing.
  Missing evidence is never treated as false."
- Step 2: `run_solution`; toast "Solution run started"; the Overview's Plan title becomes "Running." then
  "Delivering." or "Last run failed at <stage>." (red).
- Panels as the engine answers: "Decision Policy" (**Version**, **Autonomy**, **Score threshold**, "Tiers
  and Thresholds"), "Experiment and Holdout", "Actions and A1 Queue", "Monitor", "ROI Readout". Each has
  **View JSON**.
- With no decisions yet: "No decisions have been produced yet. Run the solution to populate delivered,
  held and approval-pending actions."
- ROI not claimable: "ROI cannot be claimed yet." with a reason or "No measured reason is available."

#### DSGN-058 · Propose a policy version · P2
As: owner · Needs: DSGN-057

1. Open "Propose a New Policy Version". Replace the JSON with `1` and press **Create policy version**
   with a **Reason** and the box ticked.
2. Restore the JSON, keep the reason, tick "I approve creating this policy version.", press the button.

Expect:
- Step 1: "The policy configuration must be a JSON object."
- Step 2: `update_policy`; toast "Policy version created for qa-<MMDD>-sol"; **Version** goes up.
- Without the box ticked the browser refuses the form.

#### DSGN-059 · Approve an A1 decision and record an outcome · P2
As: owner · Needs: DSGN-057 with decisions

1. In "A1 Queue", press **Approve A1**, then **Cancel**; again, then **Approve A1** in the dialog.
2. In "Latest Actions", open **Record outcome**, fill **Outcome**, **Value** `x` (if allowed),
   **Currency**, **Occurred at**, press **Record**; then a number.

Expect:
- The dialog "Approve this A1 decision?" says "Approving changes the decision for <entity> (<action>) to
  approved. It does not deliver an action by itself."; confirming sends `approve_decision` and toasts
  "A1 decision approved".
- The panel's footer says "Approvals change the decision state; they do not silently deliver an action."
- A value that is not a number gives "The outcome value must be a number."; a number sends
  `record_outcome` with `source: "manual"` and toasts "Outcome recorded".
- Empty queues say "No held or approval-pending decisions are recorded for this solution." and "No
  delivered actions are recorded for this solution."

#### DSGN-060 · Solution addresses that name nothing · P2
As: owner

1. Open `/w/<W1 id>/design/solution?id=S-999`.
2. Open `/w/<W1 id>/design/solution`.

Expect:
- Step 1: "No Solution S-999" with "Neither Orama's catalogue nor this workspace has a solution by that id."
- Step 2 shows "No Solution " with an empty id and the same sentence (suspected defect 6).

#### DSGN-061 · Delete your solution · P1
As: owner · Needs: DSGN-050

1. On `qa-<MMDD>-late-payments`, press **Delete**.
2. Type `qa-<MMDD>-late`.
3. Complete the name and press **Delete solution**.

Expect:
- The dialog "Delete This Solution?" says "The solution qa-<MMDD>-late-payments is deleted from this
  workspace and cannot be restored. Projects started from it are kept." with "Type
  `qa-<MMDD>-late-payments` to confirm".
- Step 2: **Delete solution** stays disabled.
- Step 3: `delete_solution`; toast "Deleted qa-<MMDD>-late-payments"; **Design → Solutions** opens without
  it. The project `qa-<MMDD>-late-payments` is still listed.

## Methodologies

#### DSGN-062 · The Methodologies list · P0
As: owner

1. Open **Design → Methodologies**.

Expect:
- Title "Methodologies", sub "The modelling methods your workspace can run: Orama's, built into the
  engine, and your own, written in Python. Solutions compose them, pipelines run them."; header **New methodology**.
- Figures **By Orama** ("built into the engine, read-only"), **Yours** ("written in Python in this
  workspace"), **Run here** ("today, in this workspace").
- "Yours" first: "No Methodologies of Your Own Yet" with "Write one in Python for a method Orama does not
  build in, or open one of Orama's and press Duplicate to edit." and **New methodology**.
- Then one section per family, each card with the id, name, badge "By Orama", a run badge, "Makes …"
  and "Needs …".
- An engine with no registry shows only "No Methodology Registry Yet" with "This workspace's engine does
  not list its methodologies yet. It runs tabular prediction (M-1)."

#### DSGN-063 · Filter the methodologies · P2
As: owner

1. Press a family chip, then **All families**.
2. Press **Yours**. Then type `zzzz` in "Search methodologies…".

Expect:
- Step 1 keeps that family's section only.
- Step 2: the family chips disappear; the search shows "No Methodology Matches" with "Clear the search or
  a filter to see more."

#### DSGN-064 · Ask Orama drafts a methodology · P1
As: owner · Needs: agent-ask-orama Available

1. Press **New methodology**. Check that **Ask Orama** and **Create methodology** are disabled.
2. **Name** `qa-<MMDD>-late-payers`, **What it should do** `Predict which customers will pay their invoice
   late, from their history.`, **Its target** "A yes-or-no outcome". Press **Ask Orama**.
3. Wait for the draft.

Expect:
- The drawer "New Methodology" ("Python in a repository of this workspace: fit, predict and score, with
  its parameters and metrics.") shows "Without a draft, Create starts from Lake's template for the
  description and commits it."
- **Its target** offers "Let Lake choose from the description", "A yes-or-no outcome", "One of several
  categories", "An amount", "No target: find groups".
- Step 2: `agent_turn` with `agent: "agent-ask-orama"`, `kind: "methodology_draft"`; the drawer says
  "Ask Orama is writing a first version." (or with Ask Orama's reply after a colon).
- Step 3: `draft_methodology` with the `task_id`; "The Draft" with the badge "Written by Ask Orama" (or
  "Lake's template" with the reason Ask Orama's draft was not offered and its failing checks as
  "<check>: <message>"); the reason and "Create commits these files to a new private repository; edit them
  there or on the methodology's page, then Validate."; one disclosure per file, `methodology.py` open.
- `frontend/checks/methodology-draft.mjs` drives the same flow with mocks.

#### DSGN-065 · A draft without Ask Orama · P2
As: owner · Needs: a Lake where agent-ask-orama is not Available (else `blocked`)

1. Press **New methodology**, type a **Name**, press **Ask Orama**.

Expect:
- `draft_methodology` is called directly (no `agent_turn`).
- The badge "Lake's template" with "Ask Orama is not running on this Lake, so this is Lake's template."

#### DSGN-066 · Create the methodology · P1
As: owner · Needs: DSGN-064

1. Press **Create methodology**.

Expect:
- `create_methodology` with `files`; the methodology page opens with the badge "Yours", the chip "Not
  validated" and eyebrow "Design · Your Methodology".
- "Status" says "Validate it before pipelines, experiments or training can use it." and lists
  **Repository**, **Revision**.
- "Its Sandbox": **CPU**, **Memory**, **GPUs**, **Time limit** (`3600 s` by default), **Network** "None".
- "Validate" says "Not validated yet. Validate checks the manifest, fit, predict and score, and the lock
  file, then runs it on a sample in its sandbox."
- **Train** is disabled with the tooltip "Validate it first". "Runs" says "No runs yet. Validate runs it
  on a sample; Train runs it on a dataset."

#### DSGN-067 · Validate it · P1
As: owner · Needs: DSGN-066

1. Press **Validate** and wait.

Expect:
- `validate_methodology`; toast "Validating: its interface runs in the sandbox on a sample" (or a red
  "Validate found something missing").
- The chip goes "Validating" ("Its interface is running in the sandbox on a sample."), then "Usable"
  ("Validate passed for its current code.") or "Needs fixes".
- "Runs" lists a **Validate** run with **Data** "Sample"; the page checks again every 4 seconds while it runs.
- The findings list each check with a pass or fail icon (`aria-label` "Passed" or "Failed"), failed first.
- When usable: **Train** is enabled and primary, and a "Use It" panel shows **Build a pipeline with it**.

#### DSGN-068 · Edit and commit its code · P1
As: owner · Needs: DSGN-067

1. In "Code", pick `methodology.py`, add a comment line.
2. Pick another file and back; then press the commit button.

Expect:
- The panel label says "<n> files"; the file buttons are in a group named "Files".
- The button reads "Commit changes" and is disabled until a file changes; then "Commit 1 file". Edits
  survive switching files.
- `update_methodology` with `files` and `message: "Edit methodology.py"`; toast "Committed. Validate it
  again before it runs."; the chip becomes "Validate again" ("Its code changed since Validate passed.").

#### DSGN-069 · Train it · P1
As: owner · Needs: DSGN-067 (usable again), TD-DATASET

1. Press **Train**. Check **Train** in the drawer is disabled.
2. Choose **Dataset** `qa_<MMDD>_accounts`, **Target column** `churned`.
3. Type `abc` in a float parameter and press **Train**.
4. Clear it, set **Held out** to `0` and press **Train**.

Expect:
- The drawer "Train" ("Runs its code on a dataset in its sandbox. The model it trains appears in Train →
  Models.") with **Dataset** ("Choose a dataset"), **Target column** (help "What it learns to predict;
  every other column is a feature."), **Held out**, and "Parameters" with each parameter's kind and default.
- Step 3: "<parameter> takes a number."
- Step 4: `run_methodology` is sent with `holdout_fraction: 0.2`, not 0 (suspected defect 4); toast
  "Training: its code runs in its sandbox".
- The run is listed as **Train** with its state; a queued run shows its reason (for example "Queued:
  waiting for a place among this workspace's runs."); a finished one shows held-out metrics and **Open**
  (its model).

#### DSGN-070 · Edit the methodology's name and network · P2
As: owner · Needs: DSGN-066

1. Press **Edit**, tick "Its runs may reach the internet", press **Save**.
2. Press **Edit** and **Save** without changes.

Expect:
- The drawer "Edit Methodology" ("The name and description are its manifest's, committed to its
  repository."), **Network** help "Off by default: its runs reach nothing. On, they reach the internet,
  never Lake, the workspace or the cluster."
- Step 1: "Its Sandbox" shows **Network** "Reaches the internet". No toast appears (suspected defect 5).
- Step 2 still sends `update_methodology` with only the id (suspected defect 5).

#### DSGN-071 · An Orama methodology · P1
As: owner

1. Open the methodology noted as "Runs today", then the one noted as "Not yet".
2. On the first, press **Build a pipeline with it**.

Expect:
- Eyebrow "Design · Methodology M-n"; badge "By Orama" and a run badge.
- "Status": **From** "Orama, built into the engine, read-only", **Family**, **Solutions** (a count),
  **Tracked in** on the "Not yet" one.
- Main: **What it needs**, **What it makes**, **How it is measured**, **Model kinds**; "Solutions That Use
  It" as cards, or "—".
- Only the runnable one has "Build With It" ("Open a project and choose M-n under Build a Pipeline.").
- Step 2 opens **Design → Projects**.
- The header has only **Duplicate to edit**.

#### DSGN-072 · Duplicate an Orama methodology · P2
As: owner

1. On `M-1`, press **Duplicate to edit**.

Expect:
- `duplicate_methodology`; toast "Duplicated as <name>"; the copy opens as "Yours", "Not validated", with
  **Copied from** `M-1`.

#### DSGN-073 · Methodology addresses that name nothing · P2
As: owner

1. Open `/w/<W1 id>/design/methodology?id=W-00000000`.
2. Open `/w/<W1 id>/design/methodology`.

Expect:
- Step 1: "No Methodology W-00000000" with "Neither Orama's catalogue nor this workspace has a methodology
  by that id."
- Step 2 shows "No Methodology " with "This workspace's engine lists no methodology by that id."
  (suspected defect 6).

#### DSGN-074 · Delete a methodology · P1
As: owner · Needs: DSGN-072

1. On the duplicate, press **Delete**, type part of its name, then all of it, press **Delete methodology**.

Expect:
- The dialog "Delete This Methodology?" says "<name> leaves this workspace's registry and its runs stop.
  Its repository, its models and its run records are kept."
- The button stays disabled until the exact name is typed.
- `delete_methodology`; toast "Deleted <name>"; **Design → Methodologies** opens without it.

## Agents

#### DSGN-075 · The Agents page · P0
As: owner

1. Open **Design → Agents**.

Expect:
- Title "Agents", sub "Design the agents that work for you: what each is for, its prompt, its skills, the
  models and pipelines it may use. Orama's agents are read-only; duplicate one to make it your own."
- Header **Describe it to the Agent Factory** and **New agent**.
- While loading: "Loading the agents…" and a skeleton.
- Figures **Yours** ("designed in this workspace"), **By Orama** ("read-only, duplicate to edit"), **Run
  on their own** ("of yours, on a schedule").
- "Yours" with no agents: "No Agents of Your Own Yet" with "An agent does one job for you. Design one
  yourself, describe it and let the Agent Creator Agent draft it, or open one of Orama's and duplicate
  it." and **New agent**, **Describe it to the Agent Factory**.
- "By Orama" ("Read-only. Open one to see where you meet it, or duplicate it to make your own.") lists
  nine agents, each with "By Orama" and "Available" (green) or "Not running on this Lake".
- Search "Search agents…" and the chips **All**, **By Orama**, **Yours** filter; `zzzz` shows "No Agent
  Matches" with "Clear the search or the filter to see more."

#### DSGN-076 · One of Orama's agents · P1
As: owner

1. Open the **Project Agent** card.
2. Open `/w/<W1 id>/design/agent?key=qa-no-such-agent`.

Expect:
- The address has `?key=<key>`. Eyebrow "Design · Orama's Agent"; the title with "By Orama" and its state.
- "Source": **From** "Orama's Agent Library, read-only", **Key**, **Runs as** ("On agent-runtime" or "Its
  own service"). "What It Does": **What it does for you**, **Where you meet it**, **Talk to it in Ask
  Orama** ("Yes", "Not while it is not running on this Lake" or "No"), **On this Lake**.
- **Talk in Ask Orama** shows only when it runs; it opens `/?agent=<key>`. **Duplicate to edit** shows.
- Step 2: "No Such Agent" with "Orama provides no agent by that key." and **All agents**.

#### DSGN-077 · The old Agent Library address · P2
As: owner

1. Open `/w/<W1 id>/design/agent-library?x=1`.
2. Open `/w/<W1 id>/train/agents/new`.

Expect:
- Step 1 lands on `/w/<W1 id>/design/agents?x=1`.
- Step 2 lands on `/w/<W1 id>/design/agents/new` (the Agent Factory).

#### DSGN-078 · The New Agent form · P2
As: owner

1. Press **New agent** and read the drawer.
2. Choose **Model** "A frontier model (OpenAI), run by Lake", then back to "Orama base model, inside your
   workspace (trainable)".
3. Set **Where it works** to "Anywhere in this workspace".

Expect:
- The drawer "New Agent", subtitle "Its purpose and prompt, the skills it may use, its model and
  pipelines, where it works, and when it runs on its own."
- Fields: **Name** (placeholder "Renewal watcher"), **What it's for**, **Instructions** ("Its prompt: how it
  should work, and what it must never do."), **What success looks like**, **Skills** (chips: Search data,
  Generate a dataset, Train a Model, Summarize and explain, Call the API, Use drives, Use transcripts,
  Work in repositories, Load datasets, Read and publish streams, Design pipelines, Read usage and
  billing), **Model**, **Pipelines it may use**, **Where it works**, **Which project or datasets**, **Runs
  on its own** (placeholder "every Monday at 09:00"), **Starts on new messages in**.
- **Pipelines it may use** lists "Accounts churn in qa-<MMDD>-retention" as a chip (only approved pipelines).
- With the base model, **Starts on new messages in** says "Choose a frontier model to start it from a
  stream."; with the frontier model it lists the workspace's streams or "No stream in this workspace yet.
  Create one in Train → Streams."
- **Which project or datasets** is disabled for "Anywhere in this workspace".
- **Model** lists "A model trained in this workspace:" (disabled) followed by `accounts_churn_model`.

#### DSGN-079 · Create the native agent (TD-AGENT) · P0
As: owner · Needs: DSGN-012

1. In **New agent**: **Name** `qa-<MMDD>-native-agent`, **What it's for** `Flag renewals at risk a month
   ahead`, skills **Search data** and **Summarize and explain**, **Model** "Orama base model, inside your
   workspace (trainable)", grant "Accounts churn in qa-<MMDD>-retention".
2. Press **Create agent**.

Expect:
- `create_agent`; the agent page opens at `/design/agent?id=<id>` with the badge "Yours" and eyebrow
  "Design · Your Agent".
- "Definition": **What it's for**, **Skills** as chips, **Model**, **Runs** "In your workspace", **Where it
  works** "Anywhere in this workspace", **Runs on its own** "No: only in conversation".
- "Where It Is Used": the pipeline link "Accounts churn" "in qa-<MMDD>-retention", with a chip "approved <version>" when Lake pins the version.
- "Revisions": one row "#1", "Created", the author and **Current**.
- "Run and Train It" shows **Train and improve it**.

#### DSGN-080 · Create the frontier agent with a schedule (TD-AGENT) · P1
As: owner · Needs: frontier model configured

1. **New agent**: **Name** `qa-<MMDD>-frontier-agent`, **What it's for** `Summarize at-risk accounts`,
   **Model** "A frontier model (OpenAI), run by Lake", **Runs on its own** `every monday at 09:00`.
2. Press **Create agent**.

Expect:
- `create_agent`, then `schedule_agent` with the trigger.
- The page shows **Runs** "On agent-runtime" and **Runs on its own** `every monday at 09:00`; "Where It Is
  Used" says "1 schedule".
- If the schedule is refused, a red toast "The agent was created, but its schedule was not: <message>"
  and the agent still opens.
- On **Design → Agents** the card shows the clock chip with the trigger; **Run on their own** counts 1.
- "Run and Train It" says "Nothing to train: it runs on a frontier model and calls no pipeline."

#### DSGN-081 · Edit an agent and restore a revision · P1
As: owner · Needs: DSGN-079

1. Press **Edit**, change **What success looks like** to `Every at-risk renewal is flagged`, press **Save**.
2. In "Revisions", press **Restore** on "#1", then **Restore** in the dialog.

Expect:
- The "Edit Agent" drawer shows a skeleton until the agent's streams are read, then the filled form.
- Step 1: `update_agent`; toast "Agent saved as a new revision"; "Revisions" has "#2" with "What changed"
  naming the goal, and **Current** moves to it.
- Step 2: the dialog "Restore Revision #1?" says "Its definition is saved again as the newest revision.
  The revisions after it are kept."; toast "Restored revision #1"; a new newest revision appears.

#### DSGN-082 · Duplicate agents · P2
As: owner · Needs: DSGN-079

1. On `qa-<MMDD>-native-agent`, press **Duplicate**.
2. On the **Project Agent** (`?key=`), press **Duplicate to edit** twice, pressing **Create agent** each time.

Expect:
- Step 1: toast "Duplicated as qa-<MMDD>-native-agent (copy)"; the copy opens.
- Step 2: the drawer is titled "Duplicate to Edit", prefilled with "<name> (copy)" and instructions "Do
  what <name> does: <purpose>". The second time it proposes the same name again (suspected defect 14);
  record Lake's answer.

#### DSGN-083 · Links from an agent page · P2
As: owner · Needs: DSGN-080

1. On `qa-<MMDD>-frontier-agent`, press **Run it**, then **Back**; press **Runs and schedules**, the
   schedules link and the unattended runs link; then **Talk in Ask Orama**.

Expect:
- **Run it** and **Runs and schedules** open `/train/agent?id=<id>&tab=runs`; "1 schedule" opens
  `tab=schedules`; "<n> recent runs" opens `tab=runs`.
- **Talk in Ask Orama** opens `/?agent=<id>`.
- **Run it** also shows on the native agent, which cannot run unattended (suspected defect 16).

#### DSGN-084 · An agent address that names nothing · P2
As: owner

1. Open `/w/<W1 id>/design/agent`, then `/w/<W1 id>/design/agent?id=qa-no-such-id`.

Expect:
- "No Such Agent" with "Open an agent from Design → Agents.", then with "It may have been deleted, or the
  link names another workspace."; each with **All agents**.

#### DSGN-085 · The Agent Factory conversation · P1
As: owner · Needs: agent-creator Available

1. Press **Describe it to the Agent Factory**.
2. Press the example "An agent that answers questions about one dataset".
3. Wait for the answer; if it asks a question, answer it in **Message** and press **Enter**.

Expect:
- Title "Agent Factory", sub "Tell the Agent Creator Agent what you want an agent for. It asks what it
  must, then proposes the agent. Nothing is created until you approve the proposal."; header **Back to
  agents**; breadcrumb Design › Agents › Agent Factory.
- Before step 2: "Describe the agent: what it is for, what it may touch, and what done looks like." and
  three examples; the side card "How It Works".
- Step 2 sends `send_ask_message` with `kind: "factory"`; the address gets `conversation=` and `task=`;
  the header status says "working"; the bubble says "Working on it… It keeps working if you leave; this page
  checks again every 2 seconds."; a link "View this turn in Streams" shows.
- Messages are labelled "You" and "Agent Creator Agent"; the list keeps the newest in view. Status then
  reads "waiting for you" or "proposal ready".
- **Shift+Enter** adds a line; **Enter** sends.

#### DSGN-086 · Approve the Agent Factory's proposal · P1
As: owner · Needs: DSGN-085 with a proposal

1. Read the proposal, then press **Approve and create**.

Expect:
- The side card "Proposed Agent: <name>" with a type chip, its purpose, **Type**, **Stored in**, **Work
  area**, **Instructions**, **What success looks like**, **Skills**, **Trigger** ("Conversation only" when
  none), **Pipelines**, and file disclosures. The box label reads "Change something".
- `approve_agent_proposal` with `message_id` and `proposal_sha256`; toast "Created <name>." (with " A first
  check run has started." for a frontier agent with a trigger); the agent's page opens.
- A proposal with defects shows "This proposal has defects and cannot be created yet:", the list, "Tell
  the Agent Creator Agent what to change." and no **Approve and create**.
- A proposal changed since it was shown is refused with "The proposal changed since you saw it; review it
  again."

#### DSGN-087 · The Agent Factory without a ready workspace · P2
As: newcomer

1. Open `/design/agents/new`.

Expect:
- The page renders with the examples, unlike the rest of Design (suspected defect 15). Sending gives
  Lake's refusal above the box; nothing is created.

#### DSGN-088 · Delete an agent `v0.4.0+` · P1
As: owner · Needs: DSGN-082

1. On `qa-<MMDD>-native-agent (copy)`, press **Delete**, type a wrong name, then the right one, press **Delete agent**.

Expect:
- The dialog "Delete This Agent?" says "The agent <name> is deleted from this workspace, and its schedules
  and the streams that start it with it. Its running services and jobs are stopped. Its revisions are kept."
- **Delete agent** is disabled until the exact name is typed.
- `delete_agent`; toast "Deleted <name>"; **Design → Agents** opens without it. On v0.3.1, its launches
  and schedules may outlive it.

## Catalogue Icons

#### DSGN-089 · The icon gallery · P2
As: visitor, then owner

1. Open `/design/icons`.
2. Switch to the dark theme.

Expect:
- It opens for a visitor too, outside any workspace. Title "Catalogue Icons", sub "One icon for every
  solution and methodology in Orama's catalogue, shown at 20, 32 and 64 px. Methodologies take a blue
  accent, solutions amber; a solution borrows the motif of the methodology it is built on."
- "Methodologies": 21 rows `M-1` to `M-21`; "Solutions": 20 rows `S-1` to `S-12`, `B-1` to `B-8`; each at
  three sizes with its id, name and picture label.
- "Fallbacks" ("A methodology the set has not drawn takes its family's glyph; a solution of your own
  takes its original's or its methodology's; with neither, the first letter of its name."): "Family:
  tabular" … "Family: data" and "Monogram" / "A solution of your own with no methodology" / "The letter L".
- Each icon has `role="img"` and an `aria-label` equal to its tooltip.
- One column below 1024 px, two above; readable in both themes.

## Permissions

#### DSGN-090 · A reader sees Design's write buttons and is refused · P1
As: member · Needs: TD-PROJECT, DSGN-079

1. Open **Design → Projects** in `W1`, open `qa-<MMDD>-retention` and its pipeline.
2. Press **Approve** on the pipeline page (if it is proposed), else **Check**.
3. On **Design → Solutions** press **New solution** and submit a valid form.
4. On **Design → Agents** press **New agent** and submit a valid form.

Expect:
- Every write control shows as for `owner` (Design does not check permissions first).
- Each write is refused where it was made, with Lake's message (`unauthorized`): under the button, above
  the form or in the drawer. Nothing changes.
- Reading works: the lists, the project, the pipeline and the agent pages load.

#### DSGN-091 · A reader cannot delete · P2
As: member · Needs: TD-PROJECT

1. On `qa-<MMDD>-retention`, press **Delete project**, type its name, press **Delete project**.

Expect:
- Lake's refusal shows in the dialog; the project stays.

#### DSGN-092 · A writer can design · P2
As: writer

1. Create a project `qa-<MMDD>-writer`, build a pipeline without data and approve it.
2. Delete the project.

Expect:
- Each step works as for `owner`.

## Phone width and keyboard

#### DSGN-093 · Design at 390 px · P2
As: owner · Needs: TD-PROJECT

1. At 390×844, open `qa-<MMDD>-retention`, its pipeline, **Design → Opportunities** and a solution.

Expect:
- The pipeline graph scrolls sideways inside its own frame; the page does not.
- On the pipeline page the order is: banners, Graph, Ask the Project Agent, Change the Pipeline, Runs,
  then Step, Pipeline and "Approve, Check, Run and Deploy".
- Opportunity, solution, methodology and agent cards are one per row.
- The solution's page icon is 48 px.
- Pressing **Check** scrolls to the top, where the "Checks" panel is.

#### DSGN-094 · Keyboard on Design pages · P2
As: owner · Needs: TD-PROJECT

1. On the project page, Tab to the tabs and press **Enter** on **Runs**.
2. On **Pipelines**, Tab to **Start from** and press **Space** on **From n8n**.
3. On the pipeline page, Tab into the graph and press **Enter** on a step.
4. Open **New project** and press **Tab** repeatedly past the last button.

Expect:
- The tabs are links in a "Sections" navigation with `aria-current="page"` on the open one.
- **Start from** is a group of buttons with `aria-pressed`.
- A step in the graph is reached by Tab and selected with Enter, without a page load.
- Step 4: focus should stay in the drawer (G-31); the drawers have no focus trap, so focus moves to the
  page behind (suspected defect 28).

## Clean-up

#### DSGN-095 · Delete the scratch project · P1
As: owner · Needs: DSGN-009

1. On `qa-<MMDD>-scratch`, press **Delete project**.
2. Press **Delete project** in the dialog with the field empty, then with `qa-<MMDD>-scrat`.
3. Type `qa-<MMDD>-scratch` and press **Delete project**.

Expect:
- The dialog "Delete qa-<MMDD>-scratch?" says "Removes the project, its intent and its pipelines. Its runs
  and models stay listed." with "Type `qa-<MMDD>-scratch` to delete it".
- Step 2: an empty field is refused by the browser; the wrong name shows "Type the project's name to
  delete it." The button was enabled before typing (suspected defect 2).
- Step 3: `delete_project`; toast "Project qa-<MMDD>-scratch deleted"; **Design → Projects** opens without
  it; its runs are still on **Train → Runs**.
- Keep `qa-<MMDD>-retention` (`TD-PROJECT`) and the `TD-AGENT` agents for later files; [setup.md](setup.md#clean-up)
  removes them.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/design/ProjectPage.tsx:346-348` | The row's **Check and run** sends the graph id with no dataset, model or hold-out and opens the Runs list; the pipeline page's button (`PipelinePage.tsx:866-877`) sends the name and settings and opens the run | DSGN-020 |
| 2 | `frontend/src/pages/design/ProjectPage.tsx:605-660` | Project delete is its own modal: no dry run, button enabled before typing; other deletes disable it, and none uses the shared deletion dialog of `docs/ui/deletion.md` | DSGN-095 |
| 3 | `frontend/src/pages/design/PipelinePage.tsx:861-923` | Check and run and Deploy are disabled on a proposed pipeline, but Enter in a field still submits the form to the engine | DSGN-033 |
| 4 | `frontend/src/pages/design/MethodologyPage.tsx:450` | `Number(holdout) \|\| 0.2` turns a hold-out of 0, or any text, into 0.2 | DSGN-069 |
| 5 | `frontend/src/pages/design/MethodologyPage.tsx:525-533` | Edit Methodology saves without a toast, and with nothing changed still calls `update_methodology` | DSGN-070 |
| 6 | `frontend/src/pages/design/SolutionPage.tsx:154-163`, `MethodologyPage.tsx:150-162` | With no `id` the page says "No Solution " / "No Methodology " with an empty id | DSGN-060, DSGN-073 |
| 7 | `frontend/src/pages/design/SolutionPage.tsx:471` | Start This Solution adds `task=` to the project address, which the page never reads | DSGN-056 |
| 8 | `frontend/src/pages/design/SolutionPage.tsx:444,457` | A level without a numeric `level` uses its `id` as the value, which fails `parseInteger`, so Start refuses | DSGN-055 |
| 9 | `frontend/src/pages/design/SolutionPanels.tsx:74,92,249,265`; `lib/agents.ts:301-306` | Native `required` makes the client refusals ("Confirm that you reviewed the foundation.", "An agent needs a name.", …) unreachable | DSGN-056, DSGN-078 |
| 10 | `frontend/src/pages/design/logic.ts:880-897`, `SolutionForm.tsx:29-32` | Methodology parse errors show above the form, not under **Methodologies** | DSGN-049 |
| 11 | `frontend/src/pages/design/SolutionPanels.tsx:119` | `SELECT * FROM <ref>` reads every decision row and shows 100 | DSGN-057 |
| 12 | `frontend/src/pages/design/MethodologiesPage.tsx:54-62` | Any `list_methodologies` failure, an outage included, shows "No Methodology Registry Yet" | DSGN-062 |
| 13 | `frontend/src/pages/design/MethodologiesPage.tsx:78` | **Run here** counts the workspace's own methodologies; Solutions' **Run here** counts only Orama's | DSGN-062 |
| 14 | `frontend/src/pages/design/AgentPage.tsx:78,110` | Duplicating a library agent ignores taken names, so a second copy proposes the same name | DSGN-082 |
| 15 | `frontend/src/pages/design/AgentPage.tsx:44-135`, `pages/train/FactoryPage.tsx` | The library agent view and the Agent Factory have no workspace gate | DSGN-087 |
| 16 | `frontend/src/pages/design/AgentPage.tsx:251-253` | **Run it** shows on base-model agents, which cannot run unattended | DSGN-083 |
| 17 | `frontend/src/pages/design/OpportunitiesPage.tsx:166-169` | The `waiting` answer of `create_projects_from_opportunities` is ignored: no word on projects left without a pipeline | DSGN-043 |
| 18 | `frontend/src/pages/design/PipelinePage.tsx:288-291` | A failed Project Agent task read offers no **Discard**; `task=` stays | DSGN-036 |
| 19 | `frontend/src/styles/design.css:1-34` | Loaded only by Projects, Project and Solution; drawers on other Design pages may lose its label layout on a first load; its button comment is stale (`look.css:677-700`) | DSGN-064, DSGN-078 |
| 20 | `frontend/src/pages/train/agent-skills.json` | "Train a Model" is title case among sentence-case skill labels | DSGN-078 |
| 21 | `frontend/src/pages/design/ProjectsPage.tsx:143` | Filter chips say "Approved"; the table and header chips say "approved" | DSGN-009 |
| 22 | `docs/guide/agents.md` (Agent Factory) | Says "Under **Ask it**"; the box is labelled "Message" / "Change something" (`FactoryPage.tsx:167`) | DSGN-085 |
| 23 | `docs/guide/solutions.md` (Methodologies) | Says the figures include families, cards name their tracking issue, and the button is **Commit**; none matches the page | DSGN-062, DSGN-068 |
| 24 | `docs/guide/concepts.md:81` | Lists six library agents; the page lists nine | DSGN-075 |
| 25 | `docs/guide/design.md` (Approve) | Names `approve_pipeline`; the page calls `workspace_act` `approve_plan` | DSGN-012 |
| 26 | `docs/guide/design.md` (Check and run) | Says a passing gate opens the run's page; from the project's row it opens the Runs list | DSGN-020 |
| 27 | `docs/ui/deletion.md` | Lists no Design deletion, though projects, solutions, methodologies and agents can be deleted | DSGN-061, DSGN-095 |
| 28 | `frontend/src/lib/overlay.tsx:29-75` | Drawers and dialogs move focus in and back but do not keep it inside (G-31) | DSGN-094 |
