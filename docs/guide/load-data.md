# Load data

Every dataset lives in your workspace (**Train → Data**). There are three ways to bring data in.

## Upload a file

**Train → Data → Load data → Upload a file.** CSV (a header row, then one row per line) or JSONL
(one JSON object per line), up to 16 MiB. The name you give becomes the dataset's name.

- REST/MCP: `workspace_act` with `capability: "upload_dataset"` and `input: {"table_name", "format",
  "content"}`.

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
or `WITH`, read-only). REST/MCP: `workspace_sql` with `{"statement": "SELECT …"}`.
