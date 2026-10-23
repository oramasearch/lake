# Data: Datasets, Dataset and Query

This file covers **Data → Datasets** (`/train/data`) with the **Load Data** drawer and its three
tabs, one **Dataset** (`/train/dataset`) with its delete, and **Query** (`/train/query`). It makes
`TD-DATASET`, which later files use, so run DSET-001 first.

## Before you start

- Personas: `owner`, `member` (`read` on `W1`), `writer` (`write`, not `admin`, on `W1`), `newcomer`.
- Test data: `TD-CSV`, `TD-CSV-WIDE`, `TD-CSV-ODD`, `TD-BIG` (the 17 MiB file), `TD-PROJECT` (for a
  run output table; if it is not made yet, DSET-027 is `blocked`).
- `W2` must hold no dataset (for the empty state). Check it on **Data → Datasets** in `W2` first.
- For DSET-013 only: a connection `qa-<MMDD>-pg` to `TD-PG` in `W1`, made on **Data →
  Connections** ([09-data-connections-transcripts-mcp.md](09-data-connections-transcripts-mcp.md)).
  If it does not exist yet, run DSET-013 after that file, or mark it `blocked`.
- Every dataset name you type starts with `qa_<MMDD>_`.

## Datasets: loading data

#### DSET-001 · Load TD-CSV as TD-DATASET · P0
As: owner · Needs: TD-CSV

1. In `W1`, open **Data → Datasets**.
2. Press **Load data**.
3. On the **Upload** tab, choose `accounts.csv`.
4. Change **Dataset name** to `qa_<MMDD>_accounts`.
5. Press **Upload** in the drawer's footer.

Expect:
- The header shows **Query with SQL** and **Load data** once the workspace is ready.
- The drawer is titled "Load Data" with "Each becomes a dataset you can train on." and the tabs
  **Upload**, **Generate**, **From a connection**; **Upload** is selected.
- After step 3 the drop zone shows `accounts.csv` and "<size> · press to choose another", and
  **Dataset name** reads `accounts`.
- After step 5 the drawer closes and a green banner reads "Loaded 200 rows from your CSV file into
  qa_<MMDD>_accounts." The dataset name is a link.
- The address carries `loaded=qa_<MMDD>_accounts`, `rows=200` and `format=csv`.
- The table lists `qa_<MMDD>_accounts` with **Database** `datasets` and **Kind** "Training data".

#### DSET-002 · Load the wide CSV · P1
As: owner · Needs: TD-CSV-WIDE

1. **Load data**, choose `wide.csv`, name it `qa_<MMDD>_wide`, press **Upload**.

Expect:
- The banner reads "Loaded 1200 rows from your CSV file into qa_<MMDD>_wide."
- The **Datasets** figure goes up by one.

#### DSET-003 · Load JSONL and NDJSON files · P1
As: owner · Needs: TD-CSV-ODD

1. **Load data**, choose `events.jsonl`, leave **Format** on **From the file name**, name it
   `qa_<MMDD>_events`, press **Upload**.
2. **Load data**, choose `events.ndjson`, name it `qa_<MMDD>_events_nd`, press **Upload**.
3. **Load data**, choose `events.jsonl`, set **Format** to **CSV**, name it `qa_<MMDD>_events_csv`,
   press **Upload**.

Expect:
- Steps 1 and 2: the banner reads "Loaded 20 rows from your JSONL file into …".
- Step 3: the file is read as CSV. Either it loads with one column per JSON line, or Lake refuses it
  in the drawer with its message. Record which.

#### DSET-004 · The dataset name follows the file · P1
As: owner · Needs: TD-CSV-ODD

1. **Load data**, choose `My Data 2024.csv`. Read **Dataset name**.
2. Choose `order.csv` instead. Read **Dataset name**.
3. Type `qa_<MMDD>_typed` in **Dataset name**, then choose `My Data 2024.csv` again.
4. Clear the name, then choose `order.csv`.
5. Press **Cancel**.

Expect:
- Step 1: `my_data_2024`. Step 2: `order_data` (an SQL keyword gets `_data`).
- Step 3: the typed `qa_<MMDD>_typed` is kept.
- Step 4: the name fills again: `order_data`.
- The checkbox reads "If the name is taken, save it under the next free name (<name>_2)", and its
  example follows the name in the field (`order_data_2` in step 4).
- Nothing is loaded.

#### DSET-005 · A taken name is refused · P1
As: owner · Needs: TD-DATASET

1. **Load data**, choose `accounts.csv`, name it `qa_<MMDD>_accounts`, leave the checkbox clear,
   press **Upload**.

Expect:
- The drawer stays open with Lake's refusal at the top of the form (`conflict` at
  `/input/table_name`).
- The quiet note under the form reads "Otherwise a taken name is refused, and a CSV row whose field
  count differs from the header is refused with its line number."
- No new dataset appears.

#### DSET-006 · A taken name is saved under the next free name · P1
As: owner · Needs: TD-DATASET

1. **Load data**, choose `accounts.csv`, name it `qa_<MMDD>_accounts`.
2. Tick "If the name is taken, save it under the next free name (qa_<MMDD>_accounts_2)".
3. Press **Upload**.

Expect:
- The banner title reads "Loaded 200 rows from your CSV file into qa_<MMDD>_accounts_2."
- Under it: "qa_<MMDD>_accounts was already taken, so it was saved as qa_<MMDD>_accounts_2."

#### DSET-007 · Upload checks before sending · P1
As: owner · Needs: TD-CSV-ODD, TD-BIG

1. **Load data**, type `qa_<MMDD>_none` as the name, choose no file, press **Upload**.
2. Choose `empty.csv`, press **Upload**.
3. Choose the 17 MiB file, press **Upload**.

Expect:
- Steps 1 and 2: the form shows "Choose one CSV or JSONL file."
- Step 3: the form shows "The file is larger than 16 MiB."
- No `upload_dataset` request is sent for any step (Network panel).
- The drop zone's hint reads "Up to 16 MiB".

#### DSET-008 · The dataset name pattern · P2
As: owner · Needs: TD-CSV

1. **Load data**, choose `accounts.csv`, set the name to `qa-<MMDD>-dash`, press **Upload**.
2. Set it to `9qa_<MMDD>`, press **Upload**.
3. Clear it, press **Upload**.

Expect:
- Each time the browser blocks the submit on **Dataset name** and nothing is sent.
- The help under the field reads "Named after the file; change it if you like. Letters, digits and
  underscores, starting with a letter or an underscore, so SQL can query it."

#### DSET-009 · A malformed CSV is refused with its line · P1
As: owner · Needs: TD-CSV-ODD

1. **Load data**, choose `bad.csv`, name it `qa_<MMDD>_bad`, press **Upload**.
2. Choose `dup-header.csv`, name it `qa_<MMDD>_dup`, press **Upload**.

Expect:
- Step 1: the drawer shows Lake's refusal naming line 3, in the form "line 3: <n> fields, but the
  header has <m> columns".
- Step 2: Lake's refusal names the duplicated column.
- Neither dataset is created.

#### DSET-010 · A dropped file that is not CSV or JSONL · P2
As: owner · Needs: TD-CSV-ODD

1. **Load data**, drag `sheet.xlsx` onto the drop zone.
2. Name it `qa_<MMDD>_xlsx`, press **Upload**.

Expect:
- The browser's chooser would not offer `.xlsx` (accept is `.csv,.jsonl,.ndjson,text/csv`), but a
  drop is taken: the zone shows `sheet.xlsx`. This is suspected defect 1.
- Step 2: Lake refuses the content in the form. Nothing is created.

#### DSET-011 · Generate a dataset · P1
As: owner

1. **Load data**, open **Generate**.
2. If generators are listed, pick the first under **Generator**, leave **Variants per subject**
   empty, press **Generate**.
3. Open **Generate** again, type `51` in **Variants per subject**, press **Generate**.

Expect:
- With no generators: "No generators" / "This workspace offers no dataset generators.", and the
  footer has no **Generate** button. Then steps 2 and 3 are `blocked`.
- Step 2: a toast "Dataset generated", the drawer closes, and the new table appears in the list.
  Note its name for clean-up (generated names do not carry the run prefix).
- Step 3: the browser blocks the submit (the field takes 1 to 50).

#### DSET-012 · From a connection with no connection · P1
As: owner

1. In `W2`, open **Data → Datasets**, **Load data**, then **From a connection**.

Expect:
- "No connections yet" / "Connect the database your data lives in on the Connections page, then
  pull a table from it here." with **Add one in Connections**.
- The footer has **Cancel** only, no **Pull dataset**.
- **Add one in Connections** opens **Data → Connections** in `W2`.

#### DSET-013 · Pull a table from a connection · P1
As: owner · Needs: the connection `qa-<MMDD>-pg`

1. In `W1`, **Load data**, then **From a connection**.
2. Choose `qa-<MMDD>-pg` under **From**, type a `SELECT` over one of its tables in **Query**, name it
   `qa_<MMDD>_pulled`, type `1.5` in **At most this many rows**, press **Pull dataset**.
3. Change the row limit to `100`, press **Pull dataset**.

Expect:
- The note reads "Pick one of this workspace's connections. To add another, or to browse a
  connection's tables first, open Connections."
- Step 2: "The row limit must be a whole number." and nothing is sent.
- Step 3: a toast "Pulled qa_<MMDD>_pulled", the drawer closes, the dataset is listed.
- On a workspace that enforces consent the form shows "This workspace requires an approval before
  it reaches an outside database, and Lake cannot approve it." Record it as `blocked`.

#### DSET-014 · The drawer's frame · P2
As: owner

1. **Load data**. Read the footer on each tab.
2. Press **How loading data works**, then go Back.
3. **Load data**, press **Escape**. Open it again and press **Cancel**.

Expect:
- Footer: **How loading data works** on the left, **Cancel**, then the tab's action: **Upload** on
  Upload; **Generate** only when generators exist; **Pull dataset** only when connections exist.
- Step 2 opens the guide at `/docs/load-data`.
- Escape and **Cancel** close the drawer and send nothing.

## Datasets: the list

#### DSET-015 · The list, figures and Query link · P0
As: owner · Needs: TD-DATASET

1. Open **Data → Datasets** in `W1`.
2. On `qa_<MMDD>_accounts`, press **Query**.

Expect:
- Eyebrow "Data", title "Datasets", sub "The datasets your workspace trains on."
- Figures **Datasets** ("you can train on") and **Run outputs** ("tables runs wrote").
- Columns **Dataset** (name link over `datasets/<name>`), **Database**, **Kind**, then the **Query**
  button. Sorted by Dataset, A to Z.
- Under the table: "Open a dataset for its columns and their types, row count, size and first rows."
- Step 2 opens **Query** with `SELECT * FROM datasets.qa_<MMDD>_accounts` already run.

#### DSET-016 · Search and the kind filter · P1
As: owner · Needs: TD-DATASET, TD-PROJECT

1. Type `accounts` in "Search datasets…".
2. Type `zzz-nothing`.
3. Clear the search. Press **Run outputs**, then **All tables**, then **Datasets**.

Expect:
- Step 1: only names or databases containing "accounts" stay.
- Step 2: "No dataset matches" / "Clear the search or pick another filter."
- **Run outputs** shows only engine tables (databases ending `_results`, `forecasts`, `feedback`,
  `system`) with **Kind** "Run output", and the quiet banner "Tables the engine wrote as the output
  of runs. They are not offered as training data."
- Each chip shows its count. The list opens on **Datasets** by default.

#### DSET-017 · Sort the table · P2
As: owner · Needs: TD-DATASET

1. Press the **Dataset** header twice, then **Database**, then **Kind**.

Expect:
- The first press on a column sorts ascending, the second descending; `aria-sort` follows.
- **Rows** and **Size** columns do not show (the list reads names only).

#### DSET-018 · An empty workspace · P1
As: owner

1. Switch to `W2` with the workspace picker and open **Data → Datasets**.

Expect:
- Both figures read 0.
- "No datasets yet" / "Upload a file, pull a table from a database you connected, or generate one.
  Each becomes a dataset you can train on." with **Load data**, which opens the drawer.

#### DSET-019 · No workspace yet · P1
As: newcomer

1. Open `/train/data`.

Expect:
- "No Workspace Yet" / "You have no workspace yet." with **Workspaces**, or the **Start Your
  Workspace** panel, as the shared gate decides ([04-workspaces.md](04-workspaces.md)).
- The header shows no **Query with SQL** and no **Load data**.

#### DSET-020 · Old links are sent on · P2
As: owner

1. Open `/w/<W1 id>/train/data?browse=qa-<MMDD>-pg`.
2. Open `/w/<W1 id>/train/data#connections`.
3. Open `/w/<W1 id>/train/data#storage`.
4. Open `/w/<W1 id>/train/data?error=Something%20went%20wrong`.

Expect:
- Step 1 lands on the connection's page (`/connections/connection?name=qa-<MMDD>-pg`), step 2 on
  **Data → Connections**, step 3 on **Data → Drives** with the Storage tab.
- Back from steps 1 to 3 does not return to the old address.
- Step 4: a red banner reads "Something went wrong" above the figures.

## Dataset

#### DSET-021 · A dataset's page · P0
As: owner · Needs: TD-DATASET

1. On **Data → Datasets**, open `qa_<MMDD>_accounts`.

Expect:
- Title `qa_<MMDD>_accounts`, sub `datasets/qa_<MMDD>_accounts`; header **Query it** and **Train a
  model on this**.
- Figures **Rows** "200", **Size** (KiB or MiB, sub "<n> bytes"), **Columns** "7", **Copies**.
- Tab **Columns (7)** lists the columns in the CSV header's order (`account_id`, `plan`, `seats`,
  `monthly_spend`, `support_tickets`, `tenure_months`, `churned`), never A to Z.
- Each column has a **Type** chip (`integer`, `decimal`, `text` or `boolean`), **Reads as** (or "—")
  and **Empty** as a percent.
- Side: **Facts** (Id, Database, Stored as, Rows, Bytes, Partitions, Copies) and **Actions**.

#### DSET-022 · First Rows and Partitions · P1
As: owner · Needs: TD-DATASET, TD-CSV-WIDE loaded (DSET-002)

1. On `qa_<MMDD>_accounts`, open **First Rows**.
2. Open `qa_<MMDD>_wide`, then **First Rows**.
3. If a **Partitions (<n>)** tab shows, open it.

Expect:
- At most 20 rows show, with headers in the file's column order.
- The request is `workspace_sql` with `LIMIT 20`. On an engine that refuses `LIMIT`, a second read
  without it follows and the page still shows 20.
- **Partitions** shows only when the dataset has partitions.

#### DSET-023 · Facts and Actions · P1
As: owner · Needs: TD-DATASET

1. Read the **Facts** and **Actions** cards.
2. If **Keep durable** shows, press it.
3. Press **All datasets**.

Expect:
- **Stored as** reads "JSON rows". **Copies** is a plain figure, never red; with no partitions it
  reads "All" with "every node".
- **Actions** lists **Train a model on this** with "Start a project with this dataset chosen as its
  data, then build its pipeline.", **Query it**, **All datasets** and **Delete dataset**.
- **Keep durable** shows only when a partition has one copy, with "Optional. This dataset has one
  copy, which is normal for a one-node workspace; keeping more lets it survive losing a node."
  Pressing it shows a toast "Keeping qa_<MMDD>_accounts durable".
- Step 3 opens **Data → Datasets**.

#### DSET-024 · Train a model on this · P1
As: owner · Needs: TD-DATASET

1. On `qa_<MMDD>_accounts`, press **Train a model on this** in the header.
2. Close the drawer that opens. Go Back and press the one in **Actions**.

Expect:
- Both open **Design → Projects** at `/design/projects?new=1&dataset=datasets.qa_<MMDD>_accounts`
  with **New project** open, its name starting `qa_<MMDD>_accounts-model` (or the project name rule's
  form of it) and the dataset named in its description.
- Nothing is created until the person presses **Create project** ([10-design.md](10-design.md)).

#### DSET-025 · Query it · P1
As: owner · Needs: TD-DATASET

1. On `qa_<MMDD>_accounts`, press **Query it**.

Expect:
- **Query** opens with `SELECT * FROM datasets.qa_<MMDD>_accounts` in the editor and its results
  titled "200 rows".

#### DSET-026 · Column types on the odd files · P2
As: owner · Needs: DSET-003 done

1. Open `qa_<MMDD>_events`.

Expect:
- The columns are in the order the JSON lines first write each key.
- **Type** chips read only `integer`, `decimal`, `text`, `boolean`, `JSON`, `mixed`, `empty` or
  `unknown`. A column with integers and text in the first rows reads like "integer or text".

#### DSET-027 · A run output's page · P1
As: owner · Needs: TD-PROJECT

1. On **Data → Datasets**, press **Run outputs** and open one table.

Expect:
- The title has a "Run output" chip.
- A quiet banner reads "A table the engine wrote as the output of a run."
- There is no **Train a model on this**, neither in the header nor in **Actions**; **Query it**
  remains.

#### DSET-028 · A dataset that does not exist · P2
As: owner

1. Open `/w/<W1 id>/train/dataset?name=qa_<MMDD>_missing`.
2. Open `/w/<W1 id>/train/dataset` with no name.

Expect:
- Step 1: an error block "Couldn't load the dataset" with Lake's message (G-10 decides Retry).
- Step 2: the title is "Dataset" and the header has no actions. Record what the content shows; a
  read with an empty name is suspected defect 7.

#### DSET-029 · Delete a dataset: wrong text, then right · P1
As: owner · Needs: DSET-006 done (`qa_<MMDD>_accounts_2`)

1. Open `qa_<MMDD>_accounts_2`. In **Actions**, press **Delete dataset**.
2. Read the dialog. Type `qa_<MMDD>_accounts`.
3. Press **Cancel**. Open it again and type `qa_<MMDD>_accounts_2`.
4. Press **Delete dataset** in the dialog.

Expect:
- The dialog is titled "Delete qa_<MMDD>_accounts_2" with "Deleting the dataset removes it from this
  workspace. It cannot be undone."
- It shows a skeleton, then lists "200 rows (<size>)" and any search index built on it. The size is
  in KB or MB here and KiB or MiB on the page (suspected defect 2).
- The label reads "Type qa_<MMDD>_accounts_2 to confirm".
- Step 2: the red **Delete dataset** stays disabled. Step 3 (Cancel): nothing is deleted.
- Step 4: a toast "Deleted qa_<MMDD>_accounts_2" and the page returns to **Data → Datasets**,
  without that row.

#### DSET-030 · Delete the other test datasets · P2
As: owner

1. Delete `qa_<MMDD>_events_nd` and, if they exist, `qa_<MMDD>_events_csv` and the generated table
   from DSET-011, the same way as DSET-029.

Expect:
- Each goes with its toast. A dataset a run or an experiment is reading is refused in the dialog with
  Lake's message, naming them; an older engine answers `not_supported`, shown as it is.

## Query

#### DSET-031 · Run a query · P0
As: owner · Needs: TD-DATASET

1. On **Data → Datasets**, press **Query with SQL**.
2. Type `SELECT plan, COUNT(*) FROM datasets.qa_<MMDD>_accounts GROUP BY plan` and press **Run**.

Expect:
- Before step 2: eyebrow "Data", title "Query", sub "Read-only SQL over your workspace's data: one
  SELECT or WITH query at a time."; header **Datasets**; "Run a query" / "Write one SELECT or WITH
  query above; its rows appear here and the address keeps it, so you can share the link."
- The editor card reads "SQL" and "⌘ Enter runs it"; the placeholder is "SELECT plan, COUNT(*) FROM
  datasets.accounts GROUP BY plan".
- After step 2: a **Results** card titled "<n> rows", with headers `plan` then the count, in SELECT
  order.
- The address carries `statement=`.

#### DSET-032 · The 500-row cap · P1
As: owner · Needs: DSET-002 done

1. Run `SELECT * FROM datasets.qa_<MMDD>_wide`.
2. Run `SELECT * FROM datasets.qa_<MMDD>_wide LIMIT 600`.
3. Run `SELECT * FROM datasets.qa_<MMDD>_wide LIMIT 10`.

Expect:
- Step 1: the request's statement ends with `LIMIT 501`. The title reads "More than 500 rows", the
  banner reads "Showing the first 500. Add LIMIT and OFFSET to page through the rest.", and 500 rows
  show.
- Step 2: sent as written. The title reads "600 rows", the same banner shows, and 500 rows show.
- Step 3: "10 rows", no banner.

#### DSET-033 · Refused statements · P1
As: owner · Needs: TD-DATASET

1. Run `DELETE FROM datasets.qa_<MMDD>_accounts`.
2. Run `SELECT 1; SELECT 2`.
3. Run `SELECT nope FROM datasets.qa_<MMDD>_accounts`.
4. Run `SELECT * FROM datasets.qa_<MMDD>_accounts WHERE plan = 'zzz'`.

Expect:
- Steps 1 to 3: an error block "Couldn't load the query" with Lake's message: `invalid_input` at
  `/statement` for 1 and 2, and for 3 a message that names the table's columns. No **Retry**.
- Step 4: "0 rows" and "No rows" / "The query matched nothing."
- The dataset still has 200 rows afterwards.

#### DSET-034 · Keyboard, address and Database · P1
As: owner · Needs: TD-DATASET

1. In the editor type `SELECT * FROM qa_<MMDD>_accounts`, type `datasets` in **Database**, press
   ⌘+Enter (Ctrl+Enter on Windows or Linux).
2. Edit the statement without running it, then reload.
3. Run a second statement, then press Back, then Forward.
4. Empty the editor and press ⌘/Ctrl+Enter.

Expect:
- Step 1 runs; the address has `statement=` and `database=datasets`.
- Step 2: the reload shows the last statement that was run, not the edit.
- Step 3: Back shows the first statement and its results in the editor; Forward the second.
- Step 4: nothing runs.
- The editor's sr-only label is "Query" (screen reader).
- The hint names ⌘ only, though Ctrl works (suspected defect 6).

## Permissions and phone

#### DSET-035 · A reader sees, but cannot write · P1
As: member · Needs: TD-DATASET

1. Open **Data → Datasets** in `W1`, open `qa_<MMDD>_accounts`, run a query on it.
2. **Load data**, choose `accounts.csv`, name it `qa_<MMDD>_member`, press **Upload**.
3. On the dataset, press **Delete dataset**, type its name, press **Delete dataset**.

Expect:
- Step 1 works as for `owner`.
- **Load data** and **Delete dataset** are shown to a reader. Step 2 is refused in the drawer with
  `unauthorized`. Step 3 is refused in the dialog (at the dry run or the delete). Nothing changes.

#### DSET-036 · A writer can load and delete · P2
As: writer

1. Load `accounts.csv` as `qa_<MMDD>_writer`, then delete it.

Expect:
- Both succeed with the same banner and toast as for `owner`.

#### DSET-037 · Phone width · P2
As: owner · Needs: TD-DATASET · Viewport: 390×844

1. Open **Data → Datasets**, then **Load data**, then each tab.
2. Open `qa_<MMDD>_accounts` and its **First Rows** tab.
3. Open **Query** and run `SELECT * FROM datasets.qa_<MMDD>_accounts`.

Expect:
- The drawer fills the screen width; its footer buttons stay reachable.
- The datasets table, the columns table, the first rows and the results scroll inside their own box.
- On the dataset, **Facts** and **Actions** drop below the tabs.
- The Query editor and **Run** fit the width; **Database** spans the row.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/lib/ui.tsx:837-839` | A dropped file skips the picker's `accept`, so `sheet.xlsx` is taken and sent as CSV; only the server refuses it | DSET-010 |
| 2 | `frontend/src/lib/deletion.ts:30` vs `frontend/src/lib/format.ts:240` | The delete dialog shows the size in KB/MB (base 1000); the Dataset page in KiB/MiB (base 1024). The same dataset reads two ways | DSET-029 |
| 3 | `frontend/src/pages/train/DataPage.tsx` (Load Data drawer) | **Load data** and **Delete dataset** show to a `read` member, who is refused only after submitting | DSET-035 |
| 4 | `docs/guide/load-data.md` ("Look at a Dataset") | The guide's Actions list leaves out **Keep durable** and **Delete dataset** | DSET-023 |
| 5 | `docs/ui/deletion.md` | Says one dialog is used for every delete; Train → Model's standalone Delete uses another (see 11-train.md) | DSET-029 |
| 6 | `frontend/src/pages/train/QueryPage.tsx:83` | The hint says "⌘ Enter runs it"; Ctrl+Enter works too and is what Windows and Linux use | DSET-034 |
| 7 | `frontend/src/pages/train/DatasetPage.tsx` (`get_dataset` with `dataset: ""`) | With no `name`, the page still asks the engine for a dataset with an empty name | DSET-028 |
