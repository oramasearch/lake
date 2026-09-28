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

REST/MCP use `start_solution` with `{customer_id, project, solution, level, dataset, delivery,
roi_model}`, followed by `confirm_solution` with `{task_id, edits?}`.

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

REST/MCP: `run_pipeline` with `{"project", "graph", "dataset_path": "datasets.accounts",
"holdout_fraction": 0.2, "model_name": "…"}`.

## Read the report

The run page shows what the model learned from, what it leans on (a weight is reliance, not a
cause), the rows most at risk and **what the run cannot tell you**. Train → Runs lists every run by
pipeline and project.
