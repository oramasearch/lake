# Datasets and Loading Data

A **dataset** is a table in your workspace: the data its pipelines train on, its search indexes
cover and its agents read. Every dataset lives in the workspace it was loaded into; another workspace never sees it.

Open [**Data → Datasets**](/train/data) to see them. The page has two figures (Datasets, Run outputs) and the
datasets table. Its header has **Query with SQL** and **Load data**, which appear once the
workspace is ready. The databases the workspace reads are on **Data → Connections**
([Connections](/docs/connections)), and storage is the **Storage** tab of **Data → Drives**; an old
link to Datasets' connections or storage opens those pages.

## Load Your First Dataset

Go to [**Data → Datasets → Load data**](/train/data), keep **Upload** selected, choose a **CSV or
JSONL** file (at most 16 MiB) and press **Upload**. Those are the only file formats Load data
accepts; save another kind of file as CSV first. The steps are in [Upload a File](#upload-a-file).

There are three ways to bring data in, all under **Load data**, and a fourth from a drive:

| Way | What it does |
|---|---|
| **Upload** | loads one CSV or JSONL file of at most 16 MiB |
| **Generate** | makes a dataset from one of the workspace engine's generators |
| **From a connection** | pulls the result of one read-only `SELECT` from a database the workspace connects to |
| **Load as dataset** in [**Data → Drives**](/drives) | loads a CSV or JSONL file kept in a drive ([below](#load-a-file-from-a-drive)) |

Every REST and MCP call on this page names your workspace: `customer_id` is the id of the account
that owns your workspaces (yours, or your organization's) and `workspace_id` is the workspace.
[Getting Started](/docs) says where to find both.

## Upload a File

1. Open [**Data → Datasets**](/train/data) and press **Load data**.
2. Keep **Upload** selected.
3. Choose the file (CSV or JSONL, up to 16 MiB). A CSV has a header row, then one row per line; a
   JSONL file has one JSON object per line.
4. Leave the format as **From the file name**, or choose **CSV** or **JSONL**.
5. Name the dataset, for example `accounts`.
6. Tick *If the name is taken, save it under the next free name (accounts_2)* if you want that.
7. Press **Upload**.

The page returns to Datasets with a banner saying how many rows arrived, from which kind of file,
and into which dataset, with a link to it. When the name was taken and you ticked the box, it says
the dataset was saved under the next free name.

The same upload through the API is `upload_dataset` inside `workspace_act`:

`POST /v1/upload_dataset`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "table_name": "accounts", "format": "csv", "content": "account_id,plan,churned\n1,pro,false\n2,free,true\n"}
```

- Put the file's text in `content`, and `"csv"` or `"jsonl"` in `format`.
- Add `"if_exists": "rename"` to `input` to take the next free name; the answer then carries
  `renamed_from` with the name you asked for.
- The answer's `id` (`datasets/<name>`) is the id other calls take.
- From an AI agent, the MCP tool `workspace_act` takes the same JSON.

## Load a File from a Drive

A CSV or JSONL file kept in a drive loads without leaving Lake.

1. Open [**Data → Drives**](/drives), open the drive and go to the file's folder.
2. In the file's menu (**⋯**) choose **Load as dataset**.
3. Check the **Dataset name** (it starts from the file's name, as SQL can query it) and tick *If the
   name is taken, save it under the next free name* if you want that.
4. Press **Load dataset**. The page opens **Data → Datasets** with the banner that says what
   arrived.

Through the API it is one call for either kind of drive. `id` is the file's id in a
workspace-files drive and its path in a repository drive:

`POST /v1/load_drive_dataset`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "drive_id": "<drive id>", "id": "reports/churn.csv", "table_name": "churn"}
```

- `format` is `csv` or `jsonl`, taken from the file's name when you leave it out.
- `if_exists` is `upload_dataset`'s: `"rename"` takes the next free name.
- The answer is `upload_dataset`'s, with `source` naming the drive file.
- At most 16 MiB, as an upload. From an AI agent, the MCP tool `load_drive_dataset` takes the same
  JSON.

### Name Rules

- A dataset name is letters, digits and underscores, starts with a letter or an underscore, is at
  most 63 characters, and is not a SQL keyword, so `SELECT * FROM datasets.<name>` always works.
  `accounts-2026` is refused; `accounts_2026` is fine.
- A name that is taken is refused with `conflict` and nothing is loaded, unless you asked for the
  next free name.

### What a CSV Must Look Like

Lake checks a CSV before loading it and refuses it, naming the line, when:

- the header leaves a column unnamed or names one twice;
- a quoted field does not close on its line;
- a row has more or fewer fields than the header (`line 3: 3 fields, but the header has 2
  columns`).

A cell is typed as it loads: integers, decimals, booleans and text.

## Generate a Dataset

1. Press **Load data** and choose **Generate**.
2. Pick a **Generator**. The list is the workspace engine's own generators; when it has none, the
   drawer says *No generators*.
3. Optionally set **Variants per subject** (1 to 50).
4. Press **Generate**. The page confirms with *Dataset generated*.

Through the API it is `generate_dataset` inside `workspace_act`, with the generator's `kind` and
optional `variants_per_subject`. `list_dataset_generators` inside `workspace_query` lists the
generators.

`POST /v1/generate_dataset`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "kind": "<generator kind>"}
```

Generated data is for demos and tests: it is synthetic, not drawn from your records.

## Pull a Table From a Connection

A **connection** is a database the workspace reads from. Connections have their own page,
**Data → Connections** ([Connections](/docs/connections)): add one there, browse its tables, and
load one into a dataset from the table's row. Datasets offers the same pull:

1. Press **Load data** and choose **From a connection**. With no connection yet, the drawer links
   to **Connections** to add one.
2. Choose the connection under **From** (the workspace's existing connections).
3. Write one read-only **Query**, for example `SELECT account_id, plan, seats FROM accounts`.
4. Name the dataset under **Dataset name**.
5. Optionally limit **At most this many rows** (a whole number).
6. Press **Pull dataset**.

`POST /v1/create_dataset_from_query`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "connection": "warehouse", "query": "SELECT account_id, plan, seats FROM accounts", "table_name": "accounts"}
```

The credential stays with the workspace: the pull names the saved connection, so the password does
not travel again.

## Look at a Dataset

The datasets table lists each dataset with its database, kind and, when the engine reports them,
its rows and size. The filter chips switch between **Datasets** (data you can train on), **Run
outputs** and **All tables**; search filters by name.

Open a dataset for its page:

- **Figures**: Rows, Size, Columns and Copies.
- **Columns** tab: each column's **Type** (the value type SQL sees in the first rows: integer,
  decimal, text, boolean) and **Reads as** (the workspace's analysis of the column: identifier,
  categorical, numeric, datetime, free text …).
- **First Rows** tab: the first 20 rows.
- **Partitions** tab, when the dataset has partitions.
- **Facts**: database, **Stored as** (how the workspace keeps the rows, JSON rows for every
  dataset), rows, bytes, partitions and copies.
- **Actions**: **Query it** and **All datasets**.

The upload banner is where the original file format is shown; the workspace does not record it.

Through the API, `get_dataset` reads one dataset by its id:

`GET /v1/get_dataset?customer_id=<customer id>&workspace_id=<workspace id>&id=datasets/accounts`

`list_datasets` lists them all, each with an `id` of `<database>/<name>` and a `kind` (`data` or
`result`).

### Run Outputs

Tables the engine writes as the output of a run (entity pairs, churn scores, segment assignments,
decisions: everything in a `*_results` database, plus `forecasts`, `feedback` and `system`) are
**Run outputs**. They are listed apart and are not offered as training data. Their page says *A
table the engine wrote as the output of a run*.

### Keep a Dataset Durable

**Copies** is the fewest copies any part of the dataset has. When a partition has only one copy,
the dataset page offers **Keep durable**, which asks the workspace to keep more so the dataset
survives losing a node (`promote_dataset` inside `workspace_act`).

## Query a Dataset

**Query with SQL** on Datasets, or **Query it** on a dataset, opens **Query**: read-only SQL over
the workspace's data, one `SELECT` or `WITH` query at a time. ⌘ Enter (Ctrl Enter) runs it; the
statement and database stay in the address so a query can be shared. Results show the first 500
rows.

`POST /v1/workspace_sql`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "statement": "SELECT plan, COUNT(*) FROM datasets.accounts GROUP BY plan"}
```

`workspace_sql` is a read: REST also accepts it as `GET` with the same inputs as query
parameters. The MCP tool is `workspace_sql`.

The workspace speaks a subset of SQL:

- one `WHERE` comparison;
- `COUNT(*)`, `COUNT(col)`, `SUM`, `AVG`, `MIN` and `MAX`, with or without `GROUP BY` (on any
  column, including text);
- `ORDER BY`, `LIMIT` and `OFFSET`;
- no joins and no expressions.

Anything that does not start with `SELECT` or `WITH`, several statements, and writes are refused
before they reach the workspace. A column no row carries is an error that names the table's
columns.

## Delete a Dataset

1. Open the dataset from **Datasets**.
2. In **Actions**, press **Delete dataset**. The dialog lists its rows, its size and the search
   indexes built on it, which go with it.
3. Type the dataset's name and press **Delete dataset**.

- REST: `POST /v1/delete_dataset` with `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id":
  "<database>/<dataset>"}`; add `"dry_run": true` to check first.
- MCP: `delete_dataset`.

A dataset a run or an experiment is still reading is not deleted: the deletion is refused, naming
them. Wait for the runs to finish and stop the experiment, then delete it. It cannot be undone.

## Storage

Storage (buckets, filesystems and volumes for your services) is the **Storage** tab of **Data →
Drives** ([Drives](/docs/drives#storage), [Services and Storage](/docs/services#storage)). An old
link to Datasets' Storage card opens that tab.

## Permissions

| To | You need, on the account that owns the workspace |
|---|---|
| See datasets, connections, stores and query | `read` |
| Upload, generate, pull, connect, add or remove a connection, keep durable, create storage | `write` |
| Delete storage or read a store's connection | `admin` |

You hold `admin` on your own workspaces. In an organization, owners and admins hold `admin`;
members hold `read` unless they were granted more ([Grants and Effective Access](/docs/access)).

## Limits

- An upload is one file of at most 16 MiB.
- The dataset list shows names; row counts and sizes are on each dataset's page.
- A query shows at most 500 rows; the dataset page samples 20.
- Workspace reads time out after 15 seconds and actions after 60.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| `conflict` at `/input/table_name` | The name is taken. Choose another, or add `"if_exists": "rename"`. |
| `invalid_input` naming a line | The CSV is malformed at that line; fix the header, the quoting or the field count. |
| `invalid_input` at `/input/table_name` | The name is not a valid SQL name; use letters, digits and underscores. |
| `invalid_input` at `/statement` | The query does not start with `SELECT` or `WITH` (the message quotes how it starts), or holds several statements. |
| *This workspace requires an approval before it reaches an outside database* | The workspace enforces consent for outside sources, which Lake cannot give (`consent_required`). |
| `workspace_not_ready` | The workspace is starting. Wait until **Ship → Workspaces** shows it Ready, or press **Start** on the page. |
| `unauthorized` | You have `read` but not `write` on the account; ask an owner or admin. |

## Related Pages

- [Getting Started](/docs)
- [Workspaces](/docs/workspaces)
- [Drives](/docs/drives): keep files, and load a CSV from one
- [Connections](/docs/connections): the databases a workspace reads, and pulling a table
- [Services and Storage](/docs/services)
- [AI Projects and Pipelines](/docs/design): train on a dataset
- [Search](/docs/search)
- [Models and Search Indexes](/docs/ship): make a dataset searchable
