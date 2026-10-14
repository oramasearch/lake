# Data → Connections

**Status: implemented 2026-10-11 in the source tree (lake#651); not deployed.** The databases a
workspace reads have their own page under Data, between Datasets and Transcripts. Before, they were
a card on Data → Datasets; Datasets now keeps datasets only.

A connection is the workspace engine's: lake keeps nothing of it. The pages read and change it
through `workspace_query` and `workspace_act` (`list_connections`, `create_connection`,
`delete_connection`, `list_source_tables`, `create_dataset_from_query`), plus lake's own
`list_services` and `connect_service`.

## Pages

| Page | Path | Reads | Layout |
|---|---|---|---|
| Connections | `/w/<id>/connections` | `list_connections`; `list_source_tables` (`max_tables: 500`) once per connection, cached five minutes; `list_services` | Figures: Connections, Kinds, Tables reachable (the sum, `+` when a listing was cut short, and how many connections did not answer). A search over name, kind and host, then the table: name (links to the connection), Kind, Host, the Service it came from, Status (from the engine's `credential`: Ready, No credential, Not sealed, Credential unreadable), Created, and Delete. Empty: *No connections yet* with Add connection. Add connection (header) opens the drawer; Connect One of Your Services is a card below |
| Connection | `/w/<id>/connections/connection?name=<name>` | `list_connections`, `list_source_tables` (`include_columns: true`, `max_tables: 200`) | Figures: Kind and host, Tables, Status. A split: the tables (search, schema, kind, columns and types, **Load into a dataset** per row; **Load with a query** in the toolbar) on the left; the connection's settings (never the token), its created time and Delete on the right. **Load into a dataset** opens the pull drawer with `SELECT * FROM <schema>.<table>` and a dataset name from the table's |

Delete (on a row or the connection's page) is a `ConfirmButton` that needs the connection's name
typed, then calls `delete_connection`; the connection's page then returns to the list.

The forms are shared: `connections/forms.tsx` holds Add connection and the pull form, which
Datasets' **Load data → From a connection** also uses. That tab picks from the existing
connections, and with none it links to Connections to add one.

## Old links

`DataPage.tsx` sends an old Datasets link on with `replace` (`movedFromDatasets` in
`connections/connections.ts`):

| Old | Now |
|---|---|
| `/train/data?browse=<name>` | `/connections/connection?name=<name>` |
| `/train/data#connections` | `/connections` |

## Not the Network's Connections

Network → Connections ([network.md](network.md), lake#729) is a different thing: which apps may
reach which services, kept by Lake's control plane for the cluster to enforce. This page's
description says so and links it, and that page links back here.

## Navigation and search

The rail's Data group (`DATA_NAV` in `routes/nav.ts`) lists Connections after Datasets. Go to (⌘K)
and the top bar's search list it by its name, as every rail page. Breadcrumbs: Data › Connections ›
**name**.

## Verify

- `frontend/tests/connections.test.ts`: the route and its place in the rail, the old links, the
  empty state's figures, the add input (only the kind's filled fields), the typed delete, status
  words, host and service matching, and the table query.
- In a browser at 1440 and 390 px: the empty page, Add connection, a connection's page, Delete.

## Change it

- A new connector kind: `KIND_LABELS` and `connectionInput` in `connections/connections.ts`, and
  the fields in `AddConnection`.
