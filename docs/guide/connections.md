# Connections

A **connection** is a database your workspace reads from: a Postgres database (one of your own
services, or any other) or a Snowflake warehouse. From a connection you browse the database's
tables and load one into a dataset you can train on. A connection belongs to the workspace it was
added in, and its password or token is sealed in that workspace: Lake keeps no copy, and nothing
ever shows it again.

Open **Data → Connections** (between Datasets and Transcripts) to see them. Go to (⌘K) and the top
bar's search find the page by its name. An old link to the connections on **Data → Datasets**
opens this page, and an old *Browse tables* link opens that connection's page.

Every REST and MCP call on this page names your workspace: `customer_id` is the id of the account
that owns your workspaces and `workspace_id` is the workspace. Connections are the workspace
engine's, so the calls go through `workspace_query` (reads) and `workspace_act` (changes).

## The Connections Page

- **Figures:** **Connections** (how many), **Kinds** (Postgres, Snowflake) and **Tables
  reachable** (the tables every connection can read, added up; `+` when a database has more than
  the 500 counted, and the figure says how many connections did not answer).
- **The list:** each connection's name, **Kind**, **Host** (`host:port/database`, or the Snowflake
  account and database), the **Service** it was made from (when it came from **Connect One of Your
  Services**), **Status** and when it was **Created**. Search filters by name, kind and host.
- **Status** says what the workspace holds of the credential: **Ready** (sealed in the
  workspace), **No credential** (saved without a password or token), **Not sealed** (saved before
  credentials were sealed; add it again) or **Credential unreadable** (the workspace cannot open
  it; add it again).
- **Add connection** opens the drawer below; **Delete** on a row asks you to type the
  connection's name.

## Add a Connection

1. Press **Add connection**.
2. Fill in **Name** (letters, digits, `_` and `-`), **Type** (**Postgres** or **Snowflake**), the
   host and port or the Snowflake account, warehouse and role, **Database**, **Schema**, **User**
   and **Password or access token**.
3. Press **Save connection**.

Use a read-only account: only a single `SELECT` ever runs against it. The password is sent to the
workspace once, which seals it and never returns it.

`POST /v1/workspace_act`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "create_connection", "input": {"name": "warehouse", "connector": {"type": "postgres", "host": "db.example.com", "port": "5432", "user": "reader", "database": "sales", "token": "<password>"}}}
```

`list_connections` inside `workspace_query` lists them, each with its `connector` (never the
token), `credential` (`set`, `none`, `unreadable` or `plaintext`) and `created`.

### Connect One of Your Services

When the account runs a Postgres service (**Ship → Services**), the Connections page offers it
under **Connect One of Your Services**. Press **Connect** beside a Ready service, or **Refresh** for
one already connected. The workspace reaches it inside the cluster with the credential Lake already
holds; you type no password, and none is shown. Other service kinds are not connectable yet and say
so.

`POST /v1/connect_service`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "service_id": "<service id>"}
```

Add `"name"` to choose the connection's name; it defaults to the service's name. The MCP tool is
`connect_service`. Deleting the service later breaks the connection; **Ship → Services** says so
before it deletes.

## Browse a Connection's Tables

Open a connection (its name in the list) for its page:

- **Figures:** its kind and host, how many tables it can read, and its status.
- **The tables:** each table's schema, kind and columns with their types, without pulling
  anything. Search filters by schema and name. At most 200 are listed; **Load with a query**
  reaches any table.
- **The connection:** its settings (never the password or token), when it was created, and
  **Delete**.

Through the API it is `list_source_tables` inside `workspace_query`:

`POST /v1/workspace_query`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "list_source_tables", "input": {"connection": "warehouse", "include_columns": true}}
```

## Load a Table Into a Dataset

1. On a connection's page, press **Load into a dataset** on the table's row. The drawer fills in
   `SELECT * FROM <schema>.<table>` and a dataset name from the table's (or press **Load with a
   query** to write your own).
2. Change the **Query** or the **Dataset name** if you want, and optionally limit **At most this
   many rows**.
3. Press **Pull dataset**.

The same pull is **Load data → From a connection** on **Data → Datasets**
([Datasets and Loading Data](/docs/load-data)). Through the API it is `create_dataset_from_query`
inside `workspace_act`, with `connection`, `query`, `table_name` and optional `max_rows`.

## Delete a Connection

**Delete** (on its row or its page) asks you to type the connection's name, then removes it and its
sealed credential from the workspace. Datasets already loaded from it stay.

`POST /v1/workspace_act`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "delete_connection", "input": {"name": "warehouse"}}
```

## Permissions

Reading the connections and their tables needs `read` on the account; adding, loading and deleting
need `write`. The workspace must be Ready: until then the page says it is starting.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| **Tables reachable** says a connection did not answer | The database refused or did not answer the listing. Open the connection: its page names the error. |
| *This workspace requires an approval before it reaches an outside database* | The workspace enforces consent for outside sources, which Lake cannot give (`consent_required`). |
| **Credential unreadable** or **Not sealed** | Delete the connection and add it again with its password. |
| `workspace_not_ready` | The workspace is starting. Wait until **Ship → Workspaces** shows it Ready. |
| `unauthorized` | You have `read` but not `write` on the account; ask an owner or admin. |

## Related Pages

- [Datasets and Loading Data](/docs/load-data)
- [Services and Storage](/docs/services): run a Postgres for the workspace to connect
- [Workspaces](/docs/workspaces)
- [AI Projects and Pipelines](/docs/design): train on what you loaded
