# Train: Runs, Models, Experiments and Agents

This file covers the Train group except Streams: **Runs** and one **Run**, **Models** with **New
model** and one **Model**, **Experiments** with one **Experiment**, **Run Charts** and **Compare
Runs**, and **Agents** with one **Agent** and the old Agent Factory address. It makes
`TD-EXPERIMENT` (TRN-046), which later files use.

## Before you start

- Personas: `owner`, `member` (`read` on `W1`), `writer` (`write`, not `admin`, on `W1`), `newcomer`.
- Test data: `TD-DATASET`, `TD-PROJECT`, `TD-MODEL`, `TD-AGENT`, `TD-MODELFILES`.
- `W2` must hold no run, model, experiment or agent (for the empty states).
- Extra data for this file:
  - `TD-AGENT-PIPE`: an agent in `W1` granted `TD-PROJECT`'s approved pipeline (**Design → Agents**,
    [10-design.md](10-design.md)).
  - `TD-AGENT`'s frontier agent needs a trigger for TRN-072 (set **Runs on its own** in its design).
  - A drive in `W1` holding `model.json` (for TRN-028), and a model repository in `W1` holding
    `model.json` on `main` (for TRN-029). Without them those cases are `blocked`.
  - The Experiments Agent deployed on this Lake: **Design → Agents** lists it as Available. Without
    it, TRN-047 to TRN-050 and TRN-067 are `blocked` and TRN-051 applies.
  - Optional: a validated workspace methodology `W-<8 hex>` (**Design → Methodologies**) for TRN-055.
  - Tracked runs outside experiments: some engines record none. If **Tracked Runs Outside
    Experiments** is empty, TRN-053 is `blocked`; TRN-041 covers comparing trial runs.
- New model names start with `qa-<MMDD>-`; experiment names too.
- Most trainings take minutes. A case that waits for one says so; run the next case meanwhile.

## Runs

#### TRN-001 · The runs list · P0
As: owner · Needs: TD-PROJECT

1. In `W1`, open **Train → Runs**.

Expect:
- Eyebrow "Train", title "Runs", sub "Every pipeline run: what it trained, its status, and its
  report."
- Figures **Runs** ("in this workspace"), **Succeeded** (green), **Failed** (red, "open one to see
  why"), **In progress** ("running, queued or pending").
- Columns **Pipeline** (the pipeline linked, "in", the project linked, and the run id cut to 8
  characters), **Status**, **Started** (like "3h ago"; hover shows "YYYY-MM-DD HH:MM UTC"), and the
  buttons **Report** (or **Why it failed** on a failed run) and **Charts**. Newest first.
- A trial run reads "Trial <n> of experiment <name>" with the experiment linked.

#### TRN-002 · Search and status chips · P1
As: owner · Needs: TD-PROJECT

1. Type part of `TD-PROJECT`'s pipeline name in "Search runs…".
2. Type `zzz-nothing`.
3. Clear it. Press the **Failed** chip, then **All statuses**.

Expect:
- Step 1 keeps only runs whose pipeline, project, id or experiment matches.
- Step 2: "No run matches" / "Clear the search or pick another status."
- One chip per status present, Title Case with its count (for example **Succeeded 1**, **Failed 1**).
- Sorting **Pipeline**, **Status** and **Started** works both ways.

#### TRN-003 · No runs yet · P1
As: owner

1. In `W2`, open **Train → Runs**.

Expect:
- No figures and no toolbar.
- "No runs yet" / "Run a pipeline from a project." with **Open projects**, which opens **Design →
  Projects**.

#### TRN-004 · A succeeded run's report · P0
As: owner · Needs: TD-PROJECT

1. On **Train → Runs**, press **Report** on the succeeded run.

Expect:
- Title "Run of <pipeline>" with a green "succeeded" chip. Header **Workspace streams**, **Training
  charts**, **Delete**.
- Card **Report**, "What the Run Found", opens with **Score**: a stat like "Held-out AUC" with three
  decimals and "on <n> rows the model did not see", or the quiet "No held-out score was measured for
  this run." (or the in-sample wording, TRN-006).
- Then, when the report has them: "What It Learned From", "What the Model Leans On" with "A weight is
  what the model relied on, not a cause.", "Most at Risk", "Do Not Read These as Results", "What This
  Run Cannot Tell You", and the closed **The raw report**.
- Side **Facts**: Status, Pipeline, Started ("YYYY-MM-DD HH:MM UTC · <ago>"), Run.
- **Lineage**, **Outputs**, **Log** and **Trace** show when the run has them.

#### TRN-005 · Report details · P1
As: owner · Needs: TD-PROJECT

1. On the succeeded run, open **Every figure the model records**.
2. Read **Most at Risk**.
3. Open **The raw report**.
4. Read **Trace**.

Expect:
- Step 1 lists each numeric figure and `held_out_rows`.
- **Most at Risk** has columns **Rank**, **Entity**, **Risk** (three decimals) and **Evidence**:
  chips like "tenure_months = 3" and a numbered list of reasons like "support_tickets raises risk by
  0.42". No raw JSON in the table.
- Step 3 shows the whole report as formatted JSON.
- **Trace** rows show **Span**, **Time** in "<n> ms" and a bar; a failed span's bar is red.

#### TRN-006 · A failed run and its notices · P1
As: owner · Needs: TD-PROJECT

1. On **Train → Runs**, press **Why it failed** on the failed run.

Expect:
- The status chip is red "failed".
- A run that trained nothing shows the red banner "Nothing was trained." with the reason. A step
  that could not read its data shows a quiet banner with the reason.
- A report with only an in-sample AUC reads "No held-out score. In-sample AUC <x> was measured on the
  rows the model learned from and flatters it; it is not a result."
- A caveat on the main score turns the stat red and shows the banner "Do not quote" with the reason.

#### TRN-007 · The run's header links · P2
As: owner · Needs: TD-PROJECT

1. On a run, press **Training charts**. Go Back.
2. Press **Workspace streams**.

Expect:
- Step 1 opens **Run Charts** at `/train/experiment/run?run=<id>`.
- Step 2 opens **Train → Streams**.

#### TRN-008 · A run that does not exist · P2
As: owner

1. Open `/w/<W1 id>/train/run?id=qa-<MMDD>-missing`.

Expect:
- Title "Run: qa-<MMDD>-missing", no status chip, no **Delete**.
- An error block "Couldn't load the run" with Lake's message.

## Models

#### TRN-009 · The models list · P0
As: owner · Needs: TD-MODEL

1. Open **Train → Models**.

Expect:
- Eyebrow "Train", title "Models", sub "Every model of this workspace: its versions, held-out
  scores and what it learned from.", header **New model**.
- Figures **Models** ("<n> trained versions"), **Held-out score** ("measured on rows the model did
  not see"), **From experiments** ("<p> from pipelines, <s> standalone"), **Serving** ("a version
  published in Ship"; it links to **Ship → Models**).
- `TD-MODEL` is one row with two versions: **Model** (name link, kind chip, "<pipeline> in
  <project>" under it), **Latest held-out score** like "AUC 0.742 held out" or "not measured",
  **Trained by** ("2 versions, 2 runs", "latest run <id>"), **Learned from**, **Last trained**,
  **Serving** ("not published", or "published" with a `v<n>` chip).
- Under the table: banner "Model Files" with "A model repository keeps model files with their
  history (Repositories). To score or serve one here, use New model, then Bring one."

#### TRN-010 · Search and source chips · P1
As: owner · Needs: TD-MODEL

1. Type a feature name of `TD-MODEL` (for example `tenure_months`) in "Search models, features or
   ids…".
2. Type `zzz-nothing`.
3. Clear it. Press **Pipeline**, **Experiment**, **Standalone**, **No pipeline**, **Every source**.

Expect:
- Step 1 keeps `TD-MODEL`. Step 2: "No model matches" / "Clear the search or the filter to see
  more."
- Each chip shows its count and filters by where the model came from.

#### TRN-011 · No models yet · P1
As: owner

1. In `W2`, open **Train → Models**.

Expect:
- "No models yet" / "Train one from a dataset or bring one from a file. A project's pipeline run and
  an experiment add theirs here too." with **New model** and **Open projects**.

#### TRN-012 · A model's training page · P0
As: owner · Needs: TD-MODEL

1. On **Train → Models**, press `TD-MODEL`'s name.

Expect:
- The page opens at the latest version. Title is the model name with "not published" (or "v<n>
  serving" in green). Sub "Training this model: its versions, their held-out scores and what each
  learned from." Header **Serving in Ship**.
- Figures **Held-out score**, **Versions** "2" ("2 runs"), **Serving** ("none" / "no version is
  published", or "v<n>").
- **Versions** lists v2 (bold, chip "latest") and v1 (link), each with **Trained**, **Held-out
  score**, **Run**, **Serving** and, on v1, **Compare**.
- **Compare versions** "v1 and v2" shows each figure with "better" or "worse" chips on held-out
  figures.
- Side: **Facts** (Kind, Source, Project link, Version "v2 of 2", Trained, Run, Model id),
  **Weights**, **Pipeline**, **Train and promote**.

#### TRN-013 · The score chart and versions · P1
As: owner · Needs: TD-MODEL

1. Read **Held-out score over versions**. If it has figure chips, press each.
2. In **Versions**, press **v1**.
3. Press **Compare** on v2's row.

Expect:
- With two scored versions a chart shows; with one, "One version measured a held-out score; the
  chart starts with the second."; with none, "No version measured a held-out score. Retrain with
  rows held out to measure one."
- Step 2: the address's `id` is v1's; v1 is bold, Facts read "v1 of 2".
- Step 3: **Compare versions** reads "v2 and v1". **Training curves** opens **Compare Runs** with
  both runs.
- A version with only an in-sample AUC reads "AUC <x>" and "in-sample only, not held out".

#### TRN-014 · What it learned from, and its pipeline · P2
As: owner · Needs: TD-MODEL

1. Read **What it learned from** and **Pipeline**.
2. Press a "model step" link.

Expect:
- **What it learned from** shows the report's dataset ("Trained on", "Columns not used") and
  **Features** chips, or "The run report names no dataset." / "The run report could not be read, so
  its dataset is not shown."
- **Pipeline** is titled with the pipeline linked, with "model step" chips. When it cannot say which
  step: "The pipeline does not say which of its model steps trained this model:".
- Step 2 opens **Design → Pipeline** with that step selected.

#### TRN-015 · Download the weights · P1
As: owner · Needs: TD-MODEL

1. In **Weights**, read the sentence, press **Download**.
2. If **As ONNX** shows, press it.

Expect:
- The sentence reads "v<n> of <name> as a file, to keep or to score elsewhere."
- Each press downloads a file named by Lake, with a toast "Downloaded <file name>". The request is
  `export_model` with `format` `zetta`, then `onnx`.
- **As ONNX** shows only for a classifier or regressor tree model. A refusal shows in the card.

#### TRN-016 · Retrain a project's model · P1
As: owner · Needs: TD-MODEL

1. On `TD-MODEL`'s page, read **Train and promote** and press **Retrain**.

Expect:
- The note reads "Runs <pipeline> again after its checks, as a new version of this model."
- A toast "Retraining started" and the new run's page opens.
- If the pipeline was edited since approval, the card shows "Approve the pipeline first; checks run
  on an approved pipeline." and nothing starts. If a check fails, it shows "The pipeline did not pass
  its checks: <verdict>".
- When the new run finishes, **Train → Models** shows 3 versions.

#### TRN-017 · Promote a version to Ship · P1
As: owner · Needs: TD-MODEL

1. On `TD-MODEL`'s latest version, press **Promote v<n> to Ship**.
2. Read the dialog and press **Cancel**.
3. Press it again and press **Promote**.
4. Go back to the model's **Train** page.

Expect:
- The dialog is titled "Promote v<n> to Ship?" and reads "Publishes v<n>, so `predict` answers with
  it." When another version is published it adds "It unpublishes v<m>: a call naming that id is
  refused from then on."
- Cancel changes nothing.
- Step 3: a toast "v<n> is serving" and **Ship → Model** opens.
- Step 4: the title chip reads "v<n> serving", **Serving** figure "v<n>", and the card shows "v<n> is
  the version serving." in place of the button.

#### TRN-018 · A model that does not exist · P2
As: owner

1. Open `/w/<W1 id>/train/model?id=qa-<MMDD>-missing`.

Expect:
- "No such model" / "This workspace has no model version `qa-<MMDD>-missing`." with **Every model**.

## New model

#### TRN-019 · The New Model choice · P1
As: owner

1. On **Train → Models**, press **New model**.
2. Press **Escape**. Press **New model** again.

Expect:
- A dialog "New Model" with "A model of its own, with no project or pipeline to look after. Its
  versions, scores and serving work as for any model."
- Two choices: **Train one** "From a dataset, a column to predict and a methodology." and **Bring
  one** "A model file from this computer, a drive or a model repository."
- Escape closes it.

#### TRN-020 · Train one: checks before sending · P1
As: owner · Needs: TD-DATASET

1. **New model** → **Train one**. Press **Train** with every field empty.
2. Type `qa-<MMDD>-churn` in **Name**. Press **Train**.
3. Choose `datasets.qa_<MMDD>_accounts` in **Dataset**. Choose "None"/"Choose a column" in **Column
   to predict**. Press **Train**.
4. Choose `churned`. Type `1.5` in **Held-out share**. Press **Train**.
5. Type `20%`. Press **Back**.

Expect:
- The dialog is titled "Train a Model" with "It trains in this workspace and appears in Models with
  its versions and held-out scores when the run finishes."
- **Name** is focused when it opens, with placeholder "churn".
- Step 1: "Name the model." (under **Name**, and repeated as a red line: suspected defect 5).
- Step 3: "Choose the column to predict."
- Step 4: "A held-out share is between 0 and 1, such as 0.2 (or 20%)."
- Step 5: `20%` is accepted; **Back** returns to the two choices.
- **Methodology** lists "<id> · <name>" for each runnable methodology; its help shows the
  methodology's family, or "How it learns.". Changing **Dataset** clears **Column to predict**.
- **Held-out share** starts at `0.2`, with "Rows kept out of training to score it honestly, such as
  0.2. Empty: in-sample figures only, which flatter."
- No `train_model` request is sent in steps 1 to 4.

#### TRN-021 · Train one · P0
As: owner · Needs: TD-DATASET

1. **New model** → **Train one**.
2. **Name** `qa-<MMDD>-churn`, **Dataset** `datasets.qa_<MMDD>_accounts`, **Methodology** M-1 (or the
   first listed), **Column to predict** `churned`, **Held-out share** `0.2`.
3. Press **Train**.

Expect:
- The button shows a spinner. A toast "Training qa-<MMDD>-churn" and the new run's page opens.
- The request is `train_model` with `name`, `methodology`, `dataset_path` `datasets.qa_<MMDD>_accounts`,
  `target` `churned` and `holdout_fraction` 0.2.
- An engine without the capability answers `not_supported`, shown in the dialog.

#### TRN-022 · A run that has not finished · P1
As: owner · Needs: TRN-021 just done

1. On the new run's page, before it finishes, read the page. Hover **Delete**.
2. Wait on the page until the run finishes.

Expect:
- The status chip is amber ("running", "queued" or "pending"). An info banner reads "The report
  appears when the run finishes. This page checks again every 15 seconds."
- **Delete** is disabled with the tooltip "A run is deleted once it has finished".
- No `get_run_report` request is sent while it runs.
- Step 2: within about 15 seconds of finishing, the chip changes and the report appears without a
  reload.

#### TRN-023 · A model that already exists · P2
As: owner · Needs: TRN-021 done

1. **New model** → **Train one**, name it `qa-<MMDD>-churn`, fill the rest, press **Train**.

Expect:
- "A model named qa-<MMDD>-churn exists. Retrain it, or add a new version, from its page." Nothing is
  sent.

#### TRN-024 · A standalone model's page · P1
As: owner · Needs: TRN-021 finished

1. On **Train → Models**, press **Standalone**, open `qa-<MMDD>-churn`.

Expect:
- On the list its source reads "trained on its own".
- **Facts** has no **Project** row.
- Card **Standalone model**: **Dataset** (a link to the dataset), **Predicts** `churned`,
  **Methodology**, **Held out** "20% of rows", and "No project or pipeline to look after. Move it into
  a project to build a pipeline around it."
- **Train and promote** shows **Retrain**, **New version**, "Retrain trains it again on what it
  learned from; New version on new data, another column or another held-out share.", the promote
  button, **Move into a project** and **Delete**.
- The **Dataset** link opens the dataset's page (suspected defect 8 when the path uses `/`).

#### TRN-025 · Retrain and New version · P1
As: owner · Needs: TRN-024

1. Press **Retrain**.
2. Back on the model, press **New version**. Clear **Held-out share**, type `2`.
3. Type `0.3`, press **Train new version**.

Expect:
- Step 1: a toast "Retraining started" and a new run opens.
- Step 2: the dialog "New Version of qa-<MMDD>-churn" with "Train it again on new data, another
  column or another held-out share. The model keeps what you choose for its next retrain."; the
  fields are filled from what it trained on and **Methodology** is disabled. With `2`, the muted line
  reads "A held-out share is between 0 and 1, such as 0.2 (or 20%)." and **Train new version** is
  disabled.
- Step 3: a toast "Training a new version of qa-<MMDD>-churn", a new run opens, and the request
  sends only `holdout_fraction`.
- When they finish, the model has 3 versions; **Standalone model** reads "30% of rows".

#### TRN-026 · Bring one from this computer · P1
As: owner · Needs: TD-MODELFILES

1. **New model** → **Bring one**.
2. Type `qa-<MMDD>-xgb` in **Name**. On **This computer**, choose `model.json` under **Model file**.
3. Type `2` in **Held-out AUC**. Then `0.81`.
4. Press **Bring model**.

Expect:
- The dialog "Bring a Model" with "A model trained elsewhere, kept here so it is scored, served and
  compared like a trained one." and the source buttons **This computer**, **A drive**, **A model
  repository**.
- The banner "What can be brought" lists ONNX, XGBoost, LightGBM and Lake's weights and what is
  refused.
- With no file chosen, **Bring model** is disabled and the muted line reads "Choose the file."
- Step 3: `2` shows "An AUC is between 0 and 1." and keeps **Bring model** disabled.
- Step 4: a toast "Brought qa-<MMDD>-xgb" and its model page opens. **Standalone model** reads
  "Brought from a file: uploaded model.json. It has no pipeline; each file brought is a version."

#### TRN-027 · Bring an ONNX file, and files that are refused · P1
As: owner · Needs: TD-MODELFILES

1. **New model** → **Bring one**, name it `qa-<MMDD>-onnx`, choose `model.onnx`.
2. Type `a, b, a` in **Columns**. Then type the model's columns in order, comma separated.
3. Press **Bring model**.
4. **New model** → **Bring one**, name it `qa-<MMDD>-pkl`, choose `model.pkl`, press **Bring model**.

Expect:
- After choosing the `.onnx`, **Columns** help reads "An ONNX file names no columns: list the ones it
  reads, in its order, separated by commas. Without them they are f0, f1, …"
- Step 2: "a is named twice; each column once, in the model's order." and **Bring model** is disabled.
- Step 3: a toast "Brought qa-<MMDD>-onnx"; its page lists the typed columns under **Features**.
- Step 4: the dialog shows Lake's refusal, saying why. No model `qa-<MMDD>-pkl` appears.
- An empty **Name** keeps **Bring model** disabled with no sentence saying why (suspected defect 6).

#### TRN-028 · Bring one from a drive · P2
As: owner · Needs: a drive in `W1` holding `model.json`

1. **New model** → **Bring one** → **A drive**. Name it `qa-<MMDD>-drive`.
2. Press **Choose a file**, open the drive, choose `model.json`, press **Choose**.
3. Press **Bring model**.

Expect:
- The help reads "A file of any drive, in any folder: open the drive's folders or search by name. A
  shortcut to a file is followed." With no drive: "This workspace has no drive yet." and the button
  is disabled.
- The picker is titled "Choose a Model File" with "Open a drive and its folders, or search every
  drive by name, then choose the file."
- After step 2 the file's label shows and the button reads **Choose another file**.
- Step 3: a toast "Brought qa-<MMDD>-drive" (`load_drive_model`).

#### TRN-029 · Bring one from a model repository · P2
As: owner · Needs: a model repository in `W1` with `model.json` on `main`

1. **New model** → **Bring one** → **A model repository**. Name it `qa-<MMDD>-repo`.
2. Choose the repository. Leave **Path** `model.json` and **Revision** `main`. Press **Bring model**.

Expect:
- **Repository** lists only this workspace's repositories; with none: "This workspace has no
  repository yet."
- A toast "Brought qa-<MMDD>-repo". Its **Standalone model** card reads "Brought from a file: model
  repository <name>@<revision>: model.json. …"

#### TRN-030 · A brought model's next version · P2
As: owner · Needs: TRN-026

1. Open `qa-<MMDD>-xgb`. Press **New version**.
2. Choose `model.json` again, press **Add version**.

Expect:
- The dialog is titled "New Version of qa-<MMDD>-xgb", has no **Name**, and its left button reads
  **Cancel**. The card's note reads "Brought from a file: a new file is its next version."
- A toast "Added a version of qa-<MMDD>-xgb"; the model has 2 versions.

#### TRN-031 · Move a standalone model into a project · P2
As: owner · Needs: TRN-029 (or another brought model you can spare)

1. Open `qa-<MMDD>-repo`. Press **Move into a project**.
2. Read the dialog. Set **Project name** to `qa-<MMDD>-moved`. Press **Move into project**.

Expect:
- The dialog "Move qa-<MMDD>-repo into a project?" reads "It becomes a project in Design → Projects,
  with its pipeline and all <n> versions, so you can build a pipeline around it. It is no longer
  standalone."
- A toast "qa-<MMDD>-repo is in the project qa-<MMDD>-moved" and the project opens.
- On **Train → Models**, the model is no longer under **Standalone**.

#### TRN-032 · Delete a standalone model: wrong text, then right · P1
As: owner · Needs: TRN-027 (`qa-<MMDD>-onnx`)

1. Open `qa-<MMDD>-onnx`. Press **Delete**.
2. Type `qa-<MMDD>-ONNX`. Then press **Cancel**.
3. Press **Delete** again, type `qa-<MMDD>-onnx`, press **Delete model**.

Expect:
- The dialog "Delete qa-<MMDD>-onnx?" reads "Deletes all 1 version of qa-<MMDD>-onnx. It cannot be
  undone. The runs that trained it are kept." (with "A published version stops answering `predict`."
  when one is published), then "Type qa-<MMDD>-onnx to confirm".
- Step 2: **Delete model** stays disabled (the text is case-sensitive); Cancel deletes nothing.
- Step 3: a toast "Deleted qa-<MMDD>-onnx" and **Train → Models** opens without it.
- This dialog has no dry-run list, unlike dataset and run deletion (suspected defect 9).

#### TRN-033 · Delete a run: wrong text, then right · P1
As: owner · Needs: TRN-025's runs finished

1. Open one of `qa-<MMDD>-churn`'s retrain runs from its model's **Versions**. Press **Delete**.
2. Type the run id's first 7 characters. Then the first 8.
3. Press **Delete run**.

Expect:
- The dialog is titled "Delete This Run of <pipeline>" (or "Delete This Run") with "Deleting the run
  removes it and its report from the workspace. It cannot be undone."
- It lists "1 run and its report" and "Kept: the model it trained."
- Step 2: the button is enabled only for the exact 8 characters.
- Step 3: a toast "Run deleted" and **Train → Runs** opens without it. The model version stays on
  **Train → Models**.

## Experiments

#### TRN-034 · The experiments list · P0
As: owner

1. Open **Train → Experiments**.

Expect:
- Eyebrow "Train", title "Experiments", sub "Follow training, compare results, and promote a completed
  trial.", header **Ask the Experiments Agent** and **Start experiment**.
- Figures **Experiments**, **Running**, **Trials** ("across these experiments"), **Tracked runs**
  ("outside experiments").
- A toolbar: "Search experiments and runs…", chips **All states**, **pending**, **running**,
  **succeeded**, **failed**, **stopped**, **complete**, and a **Project** box with **Filter**.
- A card **Experiments** and a card **Tracked Runs Outside Experiments** with **Compare selected
  runs** (disabled) and "Select two to eight runs. Trial runs can also be compared from their
  experiment."
- With none: "No Experiments" / "Start an experiment, or ask the Experiments Agent to propose one."
  with **Start experiment**; "No tracked runs outside experiments."

#### TRN-035 · Start an Experiment: the form · P1
As: owner

1. Press **Start experiment**. Read the drawer.
2. Change **Methodology** to another entry, if there is one.

Expect:
- The drawer "Start an Experiment" with "Choose the pipeline, data, objective and budgets. You
  confirm the parameters before training starts."
- The quiet banner reads "Selection uses the validation split. The final test split is never used
  for tuning. Metrics are named as the run report names them: held_out_auc, held_out_log_loss,
  held_out_accuracy, held_out_rmse, held_out_mae."
- Fields: **Experiment name**, **Methodology** ("<id> · <name>", M-1 chosen), **Project**,
  **Pipeline**, **Dataset** (placeholder "database.table"), **Objective metric** (`held_out_auc`,
  with suggestions), **Direction** (**Maximize**, **Minimize**), **Validation split** (`validation`),
  **Sampler** (`tpe`, `random`, `grid`), **Pruner** (`asha`, `median`, `none`), **Trial budget** 10,
  **Parallel trials** 1, **Wall-time budget (ms; 0 is unlimited)** 0, **Cost cap (0 is unlimited)** 0,
  **Seed** 1138, **Search parameters (JSON; leave blank for methodology defaults)**, **Differential
  privacy**, **Released metrics for private experiments (comma separated)**.
- Footer **Cancel** and **Review and start experiment**.
- Step 2: any text typed before is cleared (suspected defect 10). Fill the methodology first.

#### TRN-036 · Start an Experiment: bad input · P1
As: owner · Needs: TD-PROJECT, TD-DATASET

1. **Start experiment**. Fill **Experiment name** `qa-<MMDD>-bad`, **Project**, **Pipeline** (from
   `TD-PROJECT`), **Dataset** `datasets.qa_<MMDD>_accounts`. Type `{nope` in **Search parameters**.
   Press **Review and start experiment**.
2. Clear **Search parameters**. Set **Validation split** to `test`. Press **Review and start
   experiment**, then **Confirm**.
3. Clear **Trial budget**. Press **Review and start experiment**.

Expect:
- Step 1: the drawer's notice reads "Search parameters: <the parser's message>"; no dialog.
- Step 2: the dialog "Confirm Action" reads "Start training with these parameters and budgets?" with
  the full input as JSON (including `"from_defaults": true`). After **Confirm**, Lake's refusal of the
  `test` split shows in the drawer's notice, naming the field. Nothing starts.
- Step 3: the browser blocks the submit (the field is required).

#### TRN-037 · Cancel the start · P1
As: owner · Needs: TD-PROJECT, TD-DATASET

1. Fill the form as in TRN-036 with valid values, press **Review and start experiment**.
2. Press **Cancel** in the dialog. Press it again and press **Escape**.

Expect:
- No `start_experiment` request is sent. The drawer stays open with the values.

#### TRN-038 · Start TD-EXPERIMENT · P0
As: owner · Needs: TD-PROJECT, TD-DATASET

1. **Start experiment**. **Experiment name** `qa-<MMDD>-exp`, **Methodology** M-1, **Project** and
   **Pipeline** from `TD-PROJECT`, **Dataset** `datasets.qa_<MMDD>_accounts`, **Trial budget** `4`,
   **Parallel trials** `2`. Leave the rest.
2. Press **Review and start experiment**, then **Confirm**.

Expect:
- The request is `start_experiment` with `confirm: true`.
- The experiment's page opens at `/train/experiment?experiment=<id>`, titled `qa-<MMDD>-exp` with a
  "running" chip.
- When three or more trials are complete, this is `TD-EXPERIMENT`. Run TRN-039 to TRN-045 while it
  runs.

#### TRN-039 · The experiment page · P0
As: owner · Needs: TRN-038

1. Open `qa-<MMDD>-exp` from the list.

Expect:
- Eyebrow "Train · Experiments", sub "<project> · held_out_auc (maximize)". Header **All experiments**,
  **Stop experiment** (while running), and the switch reading "Live updates" with a status like
  "Live · updated <time>".
- Figures **Best objective**, **Trials** "<n> / 4" with "<c> complete · <p> pruned · <f> failed",
  **Running**, **Cost** ("cap 0").
- Cards **Optimization History** ("No trial has completed yet." at first), **Parallel Coordinates**
  ("Axes are normalized from 0 to 1; categorical values follow first occurrence."), **Parameter
  Importance** ("Parameter importance needs completed trials." at first), each with a closed **Chart
  data** (**Exact values** under Parallel Coordinates).
- **Trial Leaderboard** with one row per trial; side **Status and Budgets** with a **Trial budget**
  meter, and **By State** chips.
- **Latest Trial Curves** below, with "The latest eight trial runs appear below. Open any trial for
  its full charts and artifacts."
- New trials and states appear without a reload while Live is on.

#### TRN-040 · Live updates · P1
As: owner · Needs: TRN-038 running

1. Press the **Live updates** switch.
2. Switch to another browser tab for 20 seconds, then come back.
3. Press it again.

Expect:
- Step 1: the switch reads "Paused" (`aria-pressed` false); the status reads "Paused"; no
  `tail_experiment_events` requests follow.
- Step 2 (with Live on): the status reads "Paused" while the tab is hidden.
- Step 3: "Live updates" and, after a cycle, "Live · updated <time>". Requests come at most every 10
  seconds and at least 1 second apart.

#### TRN-041 · The leaderboard · P1
As: owner · Needs: TRN-038 with two complete trials

1. Press the **complete** chip, then **All states**.
2. Open **Columns**, untick `cost`.
3. Press the `objective` header twice.
4. Tick two trials and press **Compare selected runs (2)**.

Expect:
- Step 1 lists only complete trials, then all again.
- Step 2 hides the cost column; reload brings it back (it is not kept).
- Step 3 sorts ascending then descending; the header's `aria-sort` follows.
- The checkboxes are named "Compare trial <n>". Step 4 opens **Compare Runs** with both runs.
- Each run id links to its **Run Charts**.

#### TRN-042 · Compare selection limits · P2
As: owner · Needs: TRN-038

1. On the leaderboard tick one trial, press **Compare selected runs (1)**.

Expect:
- The notice reads "Select two to eight runs to compare." The page stays.

#### TRN-043 · Stop a trial · P1
As: owner · Needs: TRN-038 with a running trial

1. On a running trial, press **Stop trial**. Press **Cancel**.
2. Press **Stop trial** again, then **Confirm**.

Expect:
- The dialog "Confirm Action" reads "Stop this training work? Active trials will be ended
  cooperatively." with `{"experiment_id": …, "run_id": …}`.
- Cancel sends nothing.
- Step 2: the notice reads "Training stop requested."; the trial leaves "running"; the experiment
  keeps running.

#### TRN-044 · Run Charts · P1
As: owner · Needs: TRN-038 with a complete trial

1. From the leaderboard, open a complete trial's run.
2. Change **X axis** to **Relative time**, then **Wall time**, then **Step**.
3. Move **Smoothing (EMA)** to 60%. Tick **Log scale**.
4. Press **Zoom in**, **Pan left**, **Pan right**, **Zoom out**, **Reset view**. Drag across a
   chart. Shift-drag.
5. Open **Chart data** under one chart.

Expect:
- Title "Run Charts", sub "Curves, parameters and artifacts of run <id>."; **All experiments** and the
  Live switch.
- Card **Plot** with the help "Drag a region to zoom; Shift-drag to pan. Hover for the crosshair and
  values. Non-positive values are omitted on a log axis and remain in the data table."
- **Held-Out Scores** lists the run's `held_out_*` values with four decimals, and "Measured on rows
  the model never trained on. Higher is better, except for losses and errors."; or "No held-out score
  was recorded. The run held no rows out, or it ran before Lake recorded held-out scores; its run
  report may still have one."
- Curves group by prefix ("Metrics" or "<Prefix> Metrics"); `held_out_*` are not drawn as curves.
- Step 2 redraws every chart and resets zoom; step 3 shows "60%" and smooths the curves; the toolbar
  buttons move every chart together; a zoom survives the next live refresh.
- Step 5 shows **Run**, **Step**, **Wall time**, **Raw value**, **Smoothed value**.
- **Parameter Differences** and **Artifacts** show (artifact headings like "ROC Curve · held-out rows
  · <run>"), or "No parameters recorded." / "No artifacts have been recorded."
- With no curves: "No Training Curves Yet" / "No training curves have been recorded. Live updates
  will appear here."

#### TRN-045 · Compare Runs · P1
As: owner · Needs: TRN-041 step 4

1. Read the **Compare Runs** page.
2. Edit the address to `?runs=<a>,<a>,<b>`. Then `?runs=<a>`. Then no `runs`. Then nine ids.

Expect:
- Sub "Two to eight runs' held-out scores and curves on shared charts, with their parameter
  differences and artifacts." Figures **Runs compared** "2", **Metrics** ("curves recorded"),
  **Artifacts**; card **Runs** with each run as a link to its **Run Charts**; then the same Plot,
  Held-Out Scores (one row per run, " (best)" on the best), curves and **Parameter Differences** with
  **Differs** "Yes"/"No".
- `a,a,b` compares 2 runs.
- One run: "Choose Two or More Runs to Compare" / "Only one run is chosen. Select at least one more
  in the tracked runs, then press Compare." No runs: the same title with "Select two to eight runs in
  the tracked runs, then press Compare." Nine: "Choose at Most Eight Runs to Compare" / "9 runs are
  chosen. Compare Runs shows two to eight at a time." Each has **Choose runs**, and sends no
  `compare_runs` request.

#### TRN-046 · TD-EXPERIMENT is ready · P1
As: owner · Needs: TRN-038 finished

1. Wait until `qa-<MMDD>-exp` has three complete trials. Open it.

Expect:
- **Best objective** is a number in green; **Optimization History** draws the objective and "Best
  so far"; **Parameter Importance** draws bars.
- This experiment is `TD-EXPERIMENT` for later files.

#### TRN-047 · Ask the Experiments Agent · P1
As: owner · Needs: the Experiments Agent deployed

1. Press **Ask the Experiments Agent**.
2. Type `Tune qa-<MMDD> churn on datasets.qa_<MMDD>_accounts for held-out AUC with four trials.` in
   **Goal**, press **Ask the Experiments Agent**.

Expect:
- The drawer "Run an Experiment" reads "Describe the training or tuning goal. The Experiments Agent
  proposes a complete experiment for you to edit and confirm before it starts." The **Goal**
  placeholder is "Tune a churn model for validation AUC with a bounded trial budget."
- Step 2: the drawer closes, the address gains `task=<id>`, and the card "Experiments Agent Is
  Working" shows "It keeps working if you leave; this page checks again every 2 seconds." with **View
  this turn in Streams**.
- Then either the proposal (TRN-048) or a question (TRN-050).

#### TRN-048 · Confirm the agent's proposal · P1
As: owner · Needs: TRN-047 with a proposal

1. Read the card "Proposed Experiment". Open **Why these choices**.
2. In **Experiment proposal (JSON)**, change `"name"` to `"qa-<MMDD>-agent-exp"`.
3. Press **Confirm and start experiment**.

Expect:
- The card shows the agent's reply, "Edit the experiment JSON if needed. The reviewed object is
  validated again before the agent can start it.", and the JSON.
- Step 3 starts a second turn (`agent_turn` with `proposal_task_id`, `proposal_sha256` and
  `context.confirmed_proposal`), the address takes the new task, and its result card "Experiments
  Agent Result" offers **Open the running experiment**.
- **Open the running experiment** opens `qa-<MMDD>-agent-exp`.

#### TRN-049 · A malformed proposal is not started · P2
As: owner · Needs: another proposal (repeat TRN-047)

1. Replace the JSON with `[1, 2]`. Press **Confirm and start experiment**.
2. Replace it with `{nope`. Press it again.

Expect:
- Both: the notice reads "The edited experiment proposal must be a JSON object." Nothing starts.

#### TRN-050 · Reply to the agent · P2
As: owner · Needs: TRN-047 with a question

1. Type an answer in **Reply** and press **Reply to the Experiments Agent**.

Expect:
- The card "Experiments Agent Result" shows the question; the reply starts a new turn in the same
  conversation and the address takes its task.

#### TRN-051 · The agent is not running · P2
As: owner · Needs: a Lake without the Experiments Agent

1. Press **Ask the Experiments Agent**.

Expect:
- **Goal** and the drawer's button are disabled, with "The Experiments Agent is not running on this
  Lake."
- **Start experiment** still works.

#### TRN-052 · Filters and search on the list · P1
As: owner · Needs: TD-EXPERIMENT

1. Press the **running** chip, then **complete**, then **All states**.
2. Type `TD-PROJECT`'s project name in **Project**, press **Filter**. Clear it, press **Filter**.
3. Type `qa-<MMDD>` in the search.

Expect:
- Chips and **Filter** change the address (`state=`, `project=`) and the list; reload keeps them.
- Each experiment row: name link over "<project> · <metric>", a state chip, **Best** with its
  direction, **Trials / Budget** "<n> / <max>" with "<c> complete · <p> pruned · <f> failed", and
  **Cost / Cap**.
- Search filters experiments and tracked runs together. With no match: "No Experiments" / "Clear the
  search to see more."

#### TRN-053 · Compare tracked runs · P1
As: owner · Needs: tracked runs outside experiments (see Before you start)

1. Tick one run (checkbox "Compare run <id>"), press **Compare selected runs (1)**.
2. Tick nine, press **Compare selected runs (9)**.
3. Untick to two, press **Compare selected runs (2)**.

Expect:
- Steps 1 and 2: the notice "Select two to eight runs to compare."
- Step 3 opens **Compare Runs** with the two runs.
- A run trained with differential privacy says "curves suppressed for differential privacy".

#### TRN-054 · Stop an experiment · P1
As: owner · Needs: TRN-048's experiment running (or start a second one as in TRN-038)

1. On the list, press **Stop** on that running experiment. Press **Confirm**.

Expect:
- The dialog reads "Stop this training work? Active trials will be ended cooperatively." with its
  `experiment_id`.
- The notice reads "Training stop requested." and the row's state leaves "running"; **Stop** goes.
- On its page **Stop reason** is filled and **Stop experiment** is gone.

#### TRN-055 · An experiment on your own methodology · P2
As: owner · Needs: a validated `W-<8 hex>` methodology

1. **Start experiment**. Choose the methodology marked " (yours)".

Expect:
- A banner reads "<name> is your workspace's own methodology. Each trial runs its Python code as a
  training run in its sandbox, metered like any other run, and reports its held-out metrics back. Its
  trials are compared on the rows each run holds out."
- **Project (optional)**, **Pipeline (optional)**, **Column to predict** (required when it has a
  target), **Objective metric** as a list of its metrics, **Validation split** read-only, **Pruner**
  fixed at `none` (tooltip "A runner trial reports one result, so nothing is pruned part-way"), the
  list "Parameters it searches, as its manifest declares them", **Search parameters (JSON; leave
  blank for the declared ranges)**, and no **Differential privacy**.
- Once started, each trial's row shows "Lake run" with a phase and message.

#### TRN-056 · Promote a trial · P1
As: owner · Needs: TD-EXPERIMENT

1. On a complete trial with a model, press **Promote**. Press **Confirm**.
2. Open **Train → Models**.

Expect:
- The dialog reads "Promote this completed trial’s registered model as the experiment’s project model
  version?" with `experiment_id` and `run_id`.
- The notice reads "Promoted model <model id>". A banner "Promoted" reads "Model `<id>` is this
  experiment's promoted model."; the trial's row has a green "promoted" chip; **Promoted model** is
  filled in the side card.
- On **Train → Models** that model has a "promoted" chip.

#### TRN-057 · Old and broken addresses · P2
As: owner · Needs: TD-EXPERIMENT

1. Open `/w/<W1 id>/train/experiment?study=<TD-EXPERIMENT id>`.
2. Open `/w/<W1 id>/train/experiment?experiment=qa-<MMDD>-missing`.
3. Open `/w/<W1 id>/train/experiment/run` with no `run`.

Expect:
- Step 1 opens the experiment.
- Step 2: "Experiment Not Loaded" / "The experiment could not be read. The notice above says why."
  with Lake's message in the notice.
- Step 3: the notice shows Lake's error (suspected defect 13).

#### TRN-058 · An experiment with no trials · P2
As: owner · Needs: an experiment whose trials are filtered out

1. On `TD-EXPERIMENT`, press the **pruned** chip (or **failed**) when no trial has that state.

Expect:
- The leaderboard shows its headers and no rows, with no sentence saying none match (suspected
  defect 14).

## Experiments: roles

#### TRN-059 · A reader's Experiments · P1
As: member · Needs: TD-EXPERIMENT

1. Open **Train → Experiments**, then `TD-EXPERIMENT`, then a trial's charts.
2. Press **Ask the Experiments Agent**, type a goal, press **Ask the Experiments Agent**.

Expect:
- There is no **Start experiment**, no **Stop** in the list, no **Stop experiment**, no **Stop
  trial** and no **Promote**.
- Charts, the leaderboard and Compare Runs work as for `owner`.
- Step 2 is refused (`unauthorized`) and the notice shows it.

#### TRN-060 · A writer cannot promote · P1
As: writer · Needs: TD-EXPERIMENT

1. Open `TD-EXPERIMENT`.

Expect:
- **Start experiment** shows on the list. **Stop trial** shows on running trials.
- **Promote** does not show on complete trials (it needs `admin`).

#### TRN-061 · Experiments with no workspace · P2
As: newcomer

1. Open `/train/experiments`.

Expect:
- The page has no workspace gate (suspected defect 12). Record what shows: an error block "Couldn't
  load experiments", or empty lists. No button spins forever.

## Agents

#### TRN-062 · Train → Agents · P0
As: owner · Needs: TD-AGENT, TD-AGENT-PIPE

1. Open **Train → Agents**.

Expect:
- Eyebrow "Train", title "Agents", sub "Train and improve the agents that learn: those that run on a
  model, call a pipeline or retrain on a schedule. See how each performs, launch experiments, and
  promote a better version.", header **Design an agent**.
- While loading: "Loading the agents from the workspace…".
- Figures **Agents to train** ("of <n> in this workspace"), **Trained versions** ("saved by
  training"), **Stale** ("tools or goals changed since"), **Due** ("their schedule says retrain").
- One card for the native agent and one for `TD-AGENT-PIPE`; the frontier agent with no pipeline is
  not listed. Each card names why ("model", "1 pipeline"), its status ("Trained", "Untrained", "Stale"
  or "Due"), its purpose, and chips ("<n> versions", "best held-out 0.812", "a better version
  exists").
- A card opens that agent's **Train → Agent** page.

#### TRN-063 · Search and empty states · P2
As: owner

1. Type `zzz-nothing` in "Search agents…".
2. In `W2`, open **Train → Agents**.

Expect:
- Step 1: "No agent matches" / "Clear the search."
- Step 2: "No Agent to Train Yet" with **Design an agent** and "An agent that runs on the Orama base
  model, calls a pipeline or retrains on a schedule appears here. Design one first."
- In a workspace whose only agents are frontier agents with no pipeline, it reads "This workspace's
  <n> agents run on a frontier model and call no pipeline, so there is nothing to train. …" (with
  one: suspected defect 15).

#### TRN-064 · An agent's Training tab · P0
As: owner · Needs: TD-AGENT

1. Open the native agent's card.

Expect:
- Title the agent's name with a status chip; sub its purpose; header **Edit its design** and **Talk
  in Ask Orama**.
- Tabs **Training** (selected), **Unattended Runs**; no **Schedules** on a native agent.
- Figures **Held-out score**, **Versions**, **Task success** ("<n>%" or "—", "no finished runs yet"),
  **Latency** ("median run, start to finish").
- Card "Held-Out Score Over Time": a chart, or "No Evaluated Version Yet" / "Each trained version's
  held-out score is plotted here once it is evaluated."
- Card "Versions and the Data They Learned From": rows, or "Not Trained Yet" / "Set a training
  schedule below, or run an experiment, and each version it trains is listed here."
- Cards "Improve It" and "When and What It Retrains On".
- Side: **Facts** (Engagement, Model, Runs "In your workspace", Work area, Runs on its own, Id),
  **Design** with "Its purpose, prompt, skills, model and pipelines are designed in Design → Agents,
  where every revision is kept." and the usage card.

#### TRN-065 · Promote and roll back a version · P1
As: owner · Needs: the native agent with two or more trained versions (else `blocked`)

1. On an older version press **Roll back**; read the dialog; press **Cancel**.
2. Press **Roll back** again and confirm. Then press **Promote** on the newer one and confirm.

Expect:
- The dialog "Roll Back to This Version?" (or "Promote This Version?") reads "<name> will run on
  `<checkpoint>` (held-out <score>). Its design keeps a revision of the change."
- Toasts "Rolled back" and "Version promoted". The "running" chip moves to the chosen row; "best"
  stays on the highest score.

#### TRN-066 · Save a training schedule · P1
As: owner · Needs: TD-AGENT

1. In "When and What It Retrains On", set **Retrains** to **Weekly**, **Steps** `0`, leave **Training
  data** and **Start from** empty. Press **Save schedule**.
2. Reload.
3. Set **Retrains** back to **Only when you start it**, save.

Expect:
- **Retrains** offers **Only when you start it**, **When its tools or goals change**, **Daily**,
  **Weekly**, **Monthly**. Help: "0 or empty: one epoch.", "A corpus id or path; empty uses the
  built-in corpus.", "A checkpoint; empty starts from the model it runs now."
- Each save shows a toast "Training schedule saved"; step 2 keeps **Weekly**.
- After step 1, the agent's card on **Train → Agents** names "retrains weekly".

#### TRN-067 · Launch an experiment for an agent · P1
As: owner · Needs: TD-AGENT, the Experiments Agent deployed

1. In "Improve It", press **Launch experiment**.
2. Change **Compare** to each option. Type `fewer false alarms` in **What it should improve**.
3. Press **Ask the Experiments Agent**.

Expect:
- The drawer "Launch an Experiment" has the agent's name under its title and "It answers with an
  editable proposal on Train → Experiments. Nothing starts until you confirm it there."
- **Compare** offers **Its trained versions**, **Variations of its instructions**, **The models it
  could run on**. **Experiments Agent is asked** updates as you type, starting `Design an experiment
  for the agent "<name>" (id <id>) that compares …` and containing "What it should improve: fewer
  false alarms."
- Step 3 opens **Train → Experiments** with `task=<id>` and the working card.
- An experiment started from it is later listed under "Improve It".

#### TRN-068 · The pipelines an agent calls · P2
As: owner · Needs: TD-AGENT-PIPE

1. Open `TD-AGENT-PIPE`'s card. Read "The Pipelines It Calls".
2. Press **Its runs**.

Expect:
- "Its answers come from these approved pipelines. Train and compare their runs to improve it." and
  the pipeline linked with "in <project>".
- Step 2 opens **Train → Runs** with `project=` in the address, but the list is not filtered
  (suspected defect 11).

#### TRN-069 · A frontier agent: nothing to train · P1
As: owner · Needs: TD-AGENT

1. Open `/w/<W1 id>/train/agent?id=<frontier agent id>`.
2. Add `&tab=training` to the address.

Expect:
- The quiet banner "Nothing to Train" reads "This agent runs on a frontier model and calls no
  pipeline, so Lake has nothing to train. Give it the Orama base model or a pipeline in its design to
  train it here."
- Tabs **Unattended Runs** and **Schedules**; no **Training**. Step 2 falls back to **Unattended Runs**.
- **Facts** reads "On agent-runtime" under **Runs**.

#### TRN-070 · Run a frontier agent now, and stop it · P1
As: owner · Needs: TD-AGENT

1. On the frontier agent's **Unattended Runs**, type `Say hello` in **Run it once with**, press **Run
   now**.
2. While it starts, press **Stop** on its row.

Expect:
- The card reads "Unattended Runs" / "What It Did on Its Own".
- Step 1: a toast "Run started"; a row with **Started**, **How** (chip `job`), **Status** and
  **Result**; the banner "A run is starting. This page checks again every 5 seconds." while it
  starts.
- Step 2 acts at once, with no confirmation (suspected defect 16); a toast "Run stopped" and the
  status reads "Stopped".
- A finished run shows its reply (or error) under **Result**.

#### TRN-071 · Schedules · P1
As: owner · Needs: TD-AGENT

1. On the frontier agent, open **Schedules**, press **Add schedule**.
2. Type `every 1 minutes` in **When**, press **Add schedule**.
3. Type `0 9 * * 1`, `Summarize the week` in **Ask it**, press **Add schedule**.
4. Add another with `every monday at 09:00`.
5. Press **Remove** on both.

Expect:
- The card reads "When It Runs on Its Own" with "Every tick of a schedule runs this agent once, as
  you, with what you ask it here."; with none: "No schedules" / "It runs when you run it."
- The dialog "Add a Schedule" reads "Every tick runs this agent once, as you."; **When** has "In
  words, or five cron fields." and placeholder "every monday at 09:00, or 0 9 * * 1".
- Step 2: Lake's refusal (the five-minute floor) in the dialog.
- Step 3 sends `cron`, step 4 sends `trigger`. Each shows a toast "Schedule added" and a row with its
  description, cron and time zone, **Asked**, **Status** and **Last fired** "—".
- Step 5 removes at once, with no confirmation (suspected defect 16), with a toast "Schedule
  removed".

#### TRN-072 · An agent with a trigger and no runs · P2
As: owner · Needs: the frontier agent with a trigger

1. Open its **Unattended Runs** before it has run.

Expect:
- "No unattended runs yet" / "Its trigger runs it." An agent with no trigger reads "This agent only
  talks" / "It has no trigger."
- **Facts** **Engagement** reads "Conversation and unattended".

#### TRN-073 · Header links and a missing agent · P2
As: owner · Needs: TD-AGENT

1. On the native agent, press **Edit its design**. Go Back. Press **Talk in Ask Orama**.
2. Open `/w/<W1 id>/train/agent?id=qa-<MMDD>-missing`, then `/w/<W1 id>/train/agent`.

Expect:
- **Edit its design** opens **Design → Agent**; **Talk in Ask Orama** opens Ask Orama at `/?agent=<id>`.
- Step 2: "No such agent in this workspace" / "It may have been deleted, or the link names another
  workspace." with **All agents**.

#### TRN-074 · The old Agent Factory address · P1
As: owner

1. Open `/w/<W1 id>/train/agents/new?conversation=x`.
2. Press Back.

Expect:
- The address becomes `/w/<W1 id>/design/agents/new?conversation=x`; the page is titled "Agent
  Factory" (eyebrow "Design"), as in [10-design.md](10-design.md).
- Back does not return to `/train/agents/new`.

#### TRN-075 · A reader on an agent · P2
As: member · Needs: TD-AGENT

1. Open the native agent's Training tab. Press **Save schedule**.
2. Open the frontier agent, press **Run now**, then **Add schedule** with `0 9 * * 1`.

Expect:
- Every control shows. Each write is refused with Lake's message (`unauthorized`) in its form or as
  an error block; nothing changes.

## Train: roles and phone

#### TRN-076 · A reader on Runs and Models · P1
As: member · Needs: TD-PROJECT, TD-MODEL

1. Open **Train → Runs**, a run, **Train → Models**, `TD-MODEL`.
2. On the run press **Delete**, type the 8 characters, press **Delete run**.
3. On `TD-MODEL` press **Retrain**, then **Promote v<n> to Ship** and **Promote**.
4. **New model** → **Train one**, fill it, press **Train**.

Expect:
- Step 1 reads like `owner`'s.
- Steps 2 to 4 are refused in their dialog or card with `unauthorized`; nothing changes.

#### TRN-077 · A writer trains and promotes · P2
As: writer · Needs: TD-DATASET, TD-MODEL

1. **New model** → **Train one** as `qa-<MMDD>-writer`, then press **Promote v1 to Ship** on it.

Expect:
- Both succeed with the toasts of TRN-021 and TRN-017.

#### TRN-078 · New model before the workspace is ready · P2
As: newcomer

1. Open `/train/models`. Press **New model** if it shows.

Expect:
- The content shows the workspace gate (no workspace, or Start Your Workspace).
- **New model** still shows in the header (suspected defect 7). Record what the dialog shows when its
  reads fail.

#### TRN-079 · Phone width · P2
As: owner · Needs: TD-MODEL, TD-EXPERIMENT, TD-AGENT · Viewport: 390×844

1. Open **Train → Runs**, a run's report, **Train → Models**, `TD-MODEL`, then **New model** → **Train
   one** and **Bring one**.
2. Open **Train → Experiments**, **Start experiment**, then `TD-EXPERIMENT` and a trial's **Run
   Charts**.
3. Open **Train → Agents** and the native agent.

Expect:
- The New Model, Train a Model, Bring a Model and New Version dialogs fill the screen with their
  buttons pinned at the bottom, above the keyboard when typing.
- The Most at Risk, trace, versions, compare, leaderboard and tracked-runs tables scroll inside their
  own box.
- The chart toolbar shows icon-only buttons; each has its name (**Zoom in**, …) as `aria-label`.
- Charts fit the width (at least 260 px) and redraw when the phone turns.
- Agent cards stack one per row.

#### TRN-080 · Dark theme charts · P2
As: owner · Needs: TD-EXPERIMENT, TD-MODEL

1. Open `TD-EXPERIMENT`, then `TD-MODEL`, then switch the theme to dark and back.

Expect:
- Every chart redraws in the theme's colours without a reload; axis text and grid are readable.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/train/ModelPage.tsx:430-431` | **Promote** publishes, then unpublishes each other version one by one; a failure part-way leaves two versions published | TRN-017 |
| 2 | `frontend/src/pages/train/ModelPage.tsx:419-422` | **Retrain** opens `/train/run?id=` with an empty id when the answer's `run` carries no `run_id` | TRN-016 |
| 3 | `frontend/src/pages/train/RunPage.tsx:142` vs `frontend/src/pages/train/models.ts` | The held-out figures are ordered differently (log loss second on Run, last on Models), and Run ignores `holdout_auc` | TRN-004, TRN-009 |
| 4 | `frontend/src/pages/train/NewModel.tsx:147` | Train one's methodology starts as M-1 even when M-1 is not listed: the select reads "Choose a methodology" but M-1 is sent | TRN-020 |
| 5 | `frontend/src/pages/train/NewModel.tsx:204,208` | "Name the model." shows twice | TRN-020 |
| 6 | `frontend/src/pages/train/NewModel.tsx:452` | With an empty name, **Bring model** is disabled and nothing says why | TRN-027 |
| 7 | `frontend/src/pages/train/ModelsPage.tsx:32-36` | **New model** shows before the workspace is ready, unlike Datasets' header actions (and `docs/ui/train.md`) | TRN-078 |
| 8 | `frontend/src/pages/train/ModelPage.tsx:521` | The Standalone model's Dataset link splits the path on `.`; a `<db>/<name>` path (accepted by `train_model`, used in `docs/guide/train.md`) gives a broken link | TRN-024 |
| 9 | `frontend/src/pages/train/ModelPage.tsx` (`StandaloneActions`) | A standalone model's Delete uses the simple typed confirm, with no dry-run list; `docs/ui/deletion.md` says every delete uses one dialog | TRN-032 |
| 10 | `frontend/src/pages/experiments/ExperimentsPage.tsx:452` | The Start an Experiment fields are keyed by methodology, so changing **Methodology** clears what was typed | TRN-035 |
| 11 | `frontend/src/pages/train/AgentPage.tsx:217` | **Its runs** links to `/train/runs?project=`, which Runs never reads | TRN-068 |
| 12 | `frontend/src/pages/experiments/common.tsx:80`, `frontend/src/pages/train/FactoryPage.tsx` | Experiments pages and the Agent Factory have no workspace gate; with no workspace they show raw errors | TRN-061 |
| 13 | `frontend/src/pages/experiments/RunPage.tsx:17` | With no `run`, Run Charts still asks `get_run_metrics` with an empty id | TRN-057 |
| 14 | `frontend/src/pages/experiments/ExperimentPage.tsx:418` | The leaderboard has no empty state | TRN-058 |
| 15 | `frontend/src/pages/train/AgentsPage.tsx` (empty state) | With one agent: "This workspace's agent runs on a frontier model and call no pipeline" ("call" should be "calls") | TRN-063 |
| 16 | `frontend/src/pages/train/AgentPage.tsx:539` and the run **Stop** | Removing a schedule and stopping a run happen with no confirmation | TRN-070, TRN-071 |
| 17 | `frontend/src/pages/experiments/ExperimentsPage.tsx:116` | Empty number fields are read as `0`, not left out (masked today by `required` and defaults) | TRN-036 |
| 18 | `frontend/src/pages/train/AgentPage.tsx:178` | The agent's **Held-out score** figure is not rounded, unlike the cards and versions (three decimals) | TRN-064 |
| 19 | `frontend/src/pages/experiments/Chart.tsx:153,167` | The comment says ±25%; **Zoom in** halves the range and **Zoom out** doubles it | TRN-044 |
| 20 | `frontend/src/pages/experiments/common.tsx:76` | The Experiments breadcrumb's **Train** opens Runs; every other Train page's opens Agents | TRN-034 |
| 21 | `docs/guide/experiments.md` (Run an Experiment, step 5) | Says a finished turn shows "Experiments Agent Result" with the proposal; the code shows "Proposed Experiment" for a proposal and "Experiments Agent Result" otherwise | TRN-048 |
| 22 | `docs/guide/experiments.md` | Says promoted items are marked "Promoted"; the chip reads "promoted" | TRN-056 |
| 23 | `docs/ui/experiments.md` (Run an Experiment) | Says the list begins with a Run an Experiment box; it is a drawer behind **Ask the Experiments Agent** | TRN-047 |
| 24 | `docs/guide/agents.md` | Says stream-started runs show on the "Runs" tab; the tab is **Unattended Runs** | TRN-070 |
| 25 | `docs/ui/test-runbook/global-checks.md:114` (G-105) | Says Train → Runs hides a control from a reader; Runs has no write control, and Train → Run's **Delete** shows to a reader and is refused on submit | TRN-076 |
