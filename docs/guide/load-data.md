# Load data

Every dataset lives in your workspace (**Train → Data**). There are three ways to bring data in.

## Upload a file

**Train → Data → Load data → Upload a file.** CSV (a header row, then one row per line) or JSONL
(one JSON object per line), up to 16 MiB. The name you give becomes the dataset's name.

- **Names.** A dataset name is letters, digits and underscores, starting with a letter or an
  underscore, at most 63 characters, and not a SQL keyword, so `SELECT * FROM datasets.<name>`
  always works. `accounts-2026` is refused; `accounts_2026` is fine.
- **A name that is taken** is refused and nothing is loaded. Tick *If the name is taken, save it
  under the next free name* to have it saved as `accounts_2`; the page then says it was renamed.
- **Malformed CSV is refused** with the line: a header that leaves a column unnamed or names one
  twice, a quoted field that does not close on its line, or a row with more or fewer fields than
  the header (`line 3: 3 fields, but the header has 2 columns`).
- After the upload the page says how many rows arrived, from which kind of file, and into which
  dataset.
- REST/MCP: `workspace_act` with `{"customer_id": "<customer id>", "workspace_id": "<workspace
  id>", "capability": "upload_dataset", "input": {"table_name": "accounts", "format": "csv",
  "content": "<the file's text>"}}`; add `"if_exists": "rename"` to `input` to take the next free
  name. A taken name answers `conflict`; a rename answers `renamed_from` with the name you asked
  for. The answer's `id` (`datasets/<name>`) is the id `get_dataset` takes.

Every workspace call names `customer_id` and `workspace_id`; [Getting started](/docs) says where to
find them.

## Connect a database

1. Create a service: **Ship → Services → New service** (for example Postgres), or `create_service`.
2. When it is Ready, **Train → Data → Connect One of Your Services** makes it a connection
   (`connect_service`).
3. Pull a table from it into a dataset from the connection's row.

## Generate a dataset

**Train → Data → Load data → Generate a dataset** makes realistic synthetic data from a
description, useful for demos and tests (`workspace_act` with `generate_dataset`).

## Look at it

Open a dataset to see its columns and types, its first rows, and **Query it** with SQL (`SELECT`
or `WITH`, read-only). REST/MCP: `workspace_sql` with
`{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "statement": "SELECT * FROM accounts LIMIT 20"}`.

- **Type** is the value type SQL sees in the first rows (integer, decimal, text, boolean; a CSV
  cell is typed as it loads). **Reads as** is the workspace's analysis of the column (identifier,
  categorical, numeric, datetime, free text …).
- **Stored as** is how the workspace keeps the rows (JSON rows for every dataset); the workspace
  does not record which file format a dataset came from, so the upload notice is where that is
  shown.
- **Copies** is the fewest copies any part of the dataset has. With one copy the page offers *Keep
  durable*.
- **Results.** Tables the engine writes as a run's output (entity pairs, churn scores, segment
  assignments, decisions — everything in a `*_results` database, plus `forecasts`) are listed apart
  under *Results* and are not offered as training data.
- **One id.** `list_datasets` gives each dataset an `id` of `<database>/<name>` and a `kind` (`data`
  or `result`); `get_dataset` takes that id (and `<database>.<name>`).
