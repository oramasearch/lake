# Safe Mode in the Web App

**Status: implemented 2026-10-23 in the source tree (lake#906); not deployed.** What a database's
safe mode is, and the capabilities behind it, are in [api/safe-mode.md](../api/safe-mode.md).

## The control

A lock and the level ("🔒 Warn Writes"; an open lock for Off). Clicking it opens a menu titled
**Safe mode** with **Off**, **Warn Writes** and **Warn All**, each with one line of help, and a check
on the current level. Choosing one calls `set_safe_mode` and says so in a toast; a refusal is shown
under the control. The level is read with `get_safe_mode`; until it answers the control shows `…`,
and when the workspace cannot answer (an engine without safe mode) it is disabled with the reason as
its tooltip.

| Where | Applies to | Form |
|---|---|---|
| Data → Query, the editor's status bar under Run | the database in the Database field, or else the database qualifying the statement's first table (`FROM datasets.accounts`); disabled while neither names one | `status`: a quiet status-bar item; its menu opens upwards |
| Data → Connections, a **Safe mode** column | each connection | `status` |
| A connection's page, the Connection card | that connection | `field`: a button with a chevron, under a **Safe mode** label |

## The warning dialog

When a call answers `safe_mode_warning`, a dialog titled **Run This Query?** says which level is on
for which database or connection, lists the database or connection and the level, and shows every
statement exactly as it will run, in monospace. **Run** repeats the same call with the warning's
confirmation; **Cancel** runs nothing.

| Where | The call | After Cancel |
|---|---|---|
| Data → Query | `workspace_sql` (as sent, with the page's `LIMIT 501`) | a *Not run* notice with **Review**, which reopens the dialog |
| A connection's page | `list_source_tables` | the same notice above the tables |
| Load into a dataset, Load data → From a connection | `create_dataset_from_query` | the form stays open, with no error |

On Data → Connections a connection whose listing asked first is left out of *Tables reachable*
without counting as failed: the figure says how many ask first.

## Code

- `frontend/src/lib/safeMode.ts`: the levels and their words, the warning read from an error
  (`safeModeWarning`), the confirmation added to an input, the query editor's database. Pure.
- `frontend/src/pages/connections/SafeMode.tsx`: `SafeModeControl`, `SafeModeDialog`,
  `useSafeModeRead` (a read kept as a query: the dialog, the confirmation for the same read, the
  notice) and `useSafeModeAction` (an action: run, ask, run again).
- `frontend/src/lib/api.ts`: `ApiError.details` carries the warning's details.

## Verify

- `frontend/tests/safe-mode.test.ts`: the levels and the default, the warning's fields, the
  confirmation only when there is one, the editor's database, and that each page shows the control
  and the dialog. `frontend/tests/connections.test.ts`: *Tables reachable* with a connection that
  asks first.
- In a browser, against a workspace with zetta#1373: set a connection to Warn All, open its page,
  see the dialog with the catalog query, Run, and see the tables; Cancel and see the notice.
