# Train a model

## From a sentence

On a project, **Build a Pipeline** turns a sentence into a pipeline. Name the dataset and the
column to predict: *Predict the churned column of accounts from plan, monthly_spend and
support_tickets.*

Choose a **Methodology** to get that methodology's own pipeline: segmentation, forecasting, anomaly detection, survival, uplift and the rest. Only the methodologies your workspace's engine runs are listed. A pipeline whose methodology does not run yet says *Trains nothing yet* instead of pretending to train.

## Start from a solution

**Design → Solutions → Start this solution** begins a complete decision project. Choose the
dataset, where actions should go and who owns them, then enter your value per outcome, investment
and currency. These are your inputs and have no defaults.

The Project Agent reads the chosen dataset and proposes its snapshot date, entity, label and
feature columns, methodology roles and exclusion predicates. Review and edit the JSON before you
confirm it. A missing exclusion flag is called out and blocks Run; it is never assumed to be
false.

After confirmation, the project shows the solution stages, current decision policy, experiment
and holdout, actions waiting for A1 approval, delivered and held actions, monitor and ROI readout.
Policy changes need an explicit approval. A1 approvals change decision state without silently
delivering the action. Recording an outcome updates the audited ledger and ROI evidence.

REST/MCP use `start_solution` with `{customer_id, workspace_id, project, solution, level, dataset,
delivery, roi_model}`, followed by `confirm_solution` with `{task_id, workspace_id, edits?}`.

## Change it

On the pipeline page:

- **Ask the Project Agent**: describe the change (*train on accounts only and hold out 20%*). It
  reads your data and proposes edits; you see each edit and the pipeline after it, then **Apply** or
  **Discard**. Nothing changes until you apply.
- Or edit by hand: select a step to rename it, change its kind, connect or remove it; add steps and
  edges under **Change the Pipeline**.

A changed pipeline needs **Approve** again before it runs.

## Run it

**Check** runs the correctness checks without running anything. **Check and run** trains:

- **Train on**: the dataset to learn from.
- **Hold out for measuring**: 0.2 keeps a fifth of the rows out of training to measure the model.
  Setting 0 measures nothing and every score is in-sample.
- **Model name**: defaults to the project and pipeline.

REST/MCP: `run_pipeline` with `{"customer_id": "<customer id>", "workspace_id": "<workspace id>",
"project": "retention", "graph": "<pipeline name>", "dataset_path": "datasets.accounts",
"holdout_fraction": 0.2, "model_name": "retention_churn"}`.

## Run an experiment

Open **Train → Experiments** and describe the training or tuning goal in **Run an Experiment**.
ExperimentsAgent reads the available planning information and returns a complete study proposal.
Review its explanation and edit the study JSON, including objective, search space and budgets.
Nothing starts until you press **Confirm and start study**. The first agent turn has no study write;
the confirmed turn can start only the exact object you reviewed.

The study page shows trials and their metric curves as they arrive. Ask ExperimentsAgent for study
status or an explanation of the best trial through **Ask Orama**. Promotion remains a separate
confirmation.

REST uses `POST /v1/agent_turn`; MCP uses the `agent_turn` tool. Send
`{"customer_id":"<customer id>","workspace_id":"<workspace id>","agent":"agent-experiments","kind":"experiment","message":"…"}`,
follow its `task_id`, then
send the reviewed result as `context.confirmed_proposal`, with `proposal_task_id` (the proposing
turn) and `proposal_sha256` (from `get_agent_task`). Direct clients may instead use the
documented `start_study`, study/trial reads and confirmed promotion tools.

## Read the report

The run page leads with the score on rows the model did not see (for example *Held-out AUC 0.708 on
412 rows the model did not see*), marked **do not quote** when Zetta attaches a caveat to it, such
as a score measured on too few rows. A run without a holdout says it has no held-out score. Then
come what the model learned from, what it leans on (a weight is reliance, not a cause), the rows
most at risk and **what the run cannot tell you**. Train → Runs lists every run by pipeline and
project; a study's trial runs are listed as *Trial N of study* and the study's name, linked to it.

Study objectives use the run report's metric names: `held_out_auc`, `held_out_log_loss`,
`held_out_accuracy`, `held_out_rmse` and `held_out_mae`. Zetta still accepts older spellings such
as `validation.auc` and stores the canonical name.
