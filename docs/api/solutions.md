# Solutions: Orama's Catalogue and Your Own

**Status: implemented 2026-10-10 in the source tree (lake#597); not deployed.** A workspace can
start two kinds of AI solution:

- **Orama's**: the engine's catalogue, S-1 to S-23. They are **read-only**: lake refuses to edit or
  delete them, whoever asks.
- **Its own**: solutions a person writes, or duplicates from Orama's or from another of their own.
  They are kept in the workspace Zetta and can be edited and deleted.

Each has the catalogue's shape as far as a person can author it: a name, the business problem it
answers, the decision it drives, how its return is measured, the methodologies it composes, an
optional pipeline template or reference, and notes.

## Capabilities

| Capability | Kind | Input | Output |
|---|---|---|---|
| `list_solutions` | read, `read` on the space | `{customer_id, workspace_id, source?}` | `{solutions: [...], catalogue: {available, message}}` |
| `get_solution` | read, `read` on the space | `{customer_id, workspace_id, id}` | `{solution}` |
| `create_solution` | action, `write` on the space | `{customer_id, workspace_id, name, problem, decision, measurement: {metric, baseline?, target?}, methodologies?, pipeline?, notes?}` | `{solution}` |
| `update_solution` | action, `write` on the space | `{customer_id, workspace_id, id}` and any of the fields `create_solution` takes | `{solution}` |
| `delete_solution` | action, `write` on the space | `{customer_id, workspace_id, id}` | `{deleted: true}` |
| `duplicate_solution` | action, `write` on the space | `{customer_id, workspace_id, id, name?}` | `{solution}`, the new copy |

Every solution in an answer carries:

- `id`: `S-1`, … for Orama's; a UUID for one of the workspace's own.
- `source`: `orama` or `workspace`.
- `editable`: `false` for Orama's, `true` for the workspace's own.
- `name`.

An Orama solution carries the engine's catalogue fields as the engine answers them (`promise`,
`use_case`, `composition`, `pipeline_order`, `measurement`, `levels`, `runnable`, `status`, …;
zetta's `src/core/api/solution_capabilities.cpp`).

One of the workspace's own is:

```json
{
  "id": "0b6c3c5e-1f0a-4c55-9d1e-2b7f0d1e9a10",
  "source": "workspace",
  "editable": true,
  "name": "Late payments",
  "problem": "Invoices are paid late and cash runs short at month end.",
  "decision": "Which customers collections calls first each morning.",
  "measurement": {"metric": "days sales outstanding", "baseline": "52", "target": "40"},
  "methodologies": [{"id": "M-1", "minimum_level": 1}, {"id": "M-3", "minimum_level": 2}],
  "pipeline": "collections/late-payers",
  "notes": null,
  "duplicated_from": null,
  "created_at": "2026-10-10T18:04:11Z",
  "created_by": "<user id>",
  "created_by_name": "Ana Silva",
  "updated_at": null,
  "updated_by": null
}
```

- `problem`, `decision` and `measurement.metric` are required. `baseline` and `target` are text, so
  a person can write `52 days` or `12%`; an empty one is answered as `null`.
- `methodologies` is a list of methodology ids (`"M-1"`) or `{id, minimum_level}` objects, where
  `minimum_level` (1 to 4, default 1) is the level from which the solution uses it, as in the
  catalogue's `composition`. Each id is listed once. Lake does not check the ids against the
  engine's registry: a solution may name a methodology the engine does not run yet.
- `pipeline` is free text: a reference `<project>/<pipeline>`, which the page links to, or a
  description of a template.
- `duplicated_from` is the id it was copied from (`S-1`, or another of the workspace's own).
- `created_by_name` is the author's display name, else their email, read from the control store
  where identity lives; `null` for an author lake has no user for.
- `updated_at` and `updated_by` are set by the last `update_solution`.

### Listing both kinds

`list_solutions` answers Orama's first, as the engine lists them, then the workspace's own by name.
`source: "orama"` or `source: "workspace"` keeps one kind. When the engine cannot list Orama's (an
engine without the solution registry answers `unknown_capability`), the workspace's own are still
listed, and `catalogue` says why:

```json
{"solutions": [...], "catalogue": {"available": false, "message": "this workspace's engine does not list Orama's solutions yet"}}
```

### Orama's stay read-only

`update_solution` and `delete_solution` refuse any id that is not one of the workspace's own UUIDs
with `forbidden` (403), whoever asks, before any record is written:

```json
{"ok": false, "error": {"code": "forbidden", "path": "/id", "message": "S-1 (Churn & Retention) is one of Orama's catalogue solutions, which are read-only and cannot be edited. Duplicate it (duplicate_solution) to make an editable solution of your own."}}
```

`POST /v1/update_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "S-1", "name": "Mine now"}
```

The catalogue is read only to name the solution in the message: an id the catalogue does not know
answers `not_found`. Hiding the page's buttons is not the guard; the server is.

### Duplicating

`POST /v1/duplicate_solution`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "id": "S-1", "name": "Our churn"}
```

`duplicate_solution` copies either kind into a new solution of the workspace's own, named `name`,
or `"<name> (copy)"` when `name` is left out (cut to 200 bytes, never inside a character).

From one of Orama's:

| Field | From the catalogue entry |
|---|---|
| `name` | its `name` (no " (copy)": there is no solution of the workspace's own by that name yet) |
| `problem` | its `use_case`, else its `promise` |
| `decision` | its `promise`, else its `use_case` |
| `measurement.metric` | its measured outcome with its unit, `retained revenue (USD)`; else its `measurement_design`. `baseline` and `target` are left for the person |
| `methodologies` | its `composition`, each methodology once, with its `minimum_level` |
| `pipeline` | `Methodologies in order: M-2, M-1, …` from its `pipeline_order` |
| `notes` | where it came from, its measurement design and its decision policy |
| `duplicated_from` | its id |

From one of the workspace's own: every field, with the new name and `duplicated_from` set to its id.
The copy is a new record; editing it never changes the original.

## Where they live

A workspace's own solutions are sealed records in the collection `lake_solutions` of the workspace
Zetta ([workspace records](../architecture/workspace-records.md)), found by the space
(`customer:<id>`). Nothing about them is in PostgreSQL. A solution belongs to the workspace it was
created in: another workspace of the same space neither lists it nor reads, edits, deletes or
copies it (`not_found`), and another space is refused before any workspace is read
(`unauthorized`). Deleting the workspace deletes them with its Zetta.

Edits and deletes are written conditionally on the revision read and retried on a conflict, as for
transcripts. Their audit goes to the workspace's own log (`audit_route`, `src/api/audit_routing.cpp`).

Orama's catalogue is the engine's and is never copied into lake: `list_solutions` and
`get_solution` read it through the workspace's engine (`list_solutions`, `get_solution`, which
`workspace_query` also still allows).

Two capabilities share a name: lake's `create_solution` (this page) writes a solution of the
workspace's own, while the engine's `create_solution`, reached only inside `workspace_act` and called
by `confirm_solution`, composes a catalogue solution's project. The same holds for `list_solutions`
and `get_solution`: lake's answer both kinds, the engine's (inside `workspace_query`) only Orama's.

## Starting one as a project

- **Orama's:** `start_solution` and `confirm_solution` ([solutions guide](../guide/solutions.md))
  create the project, and the engine's `create_solution` composes the governed foundation: the
  pipeline, policy, holdout experiment, monitor and ROI readout.
- **Your own:** the engine's `create_solution` takes only one of Orama's catalogue ids, so lake
  cannot compose that governed foundation for a solution it does not know. **Start as a project**
  on the page does what it can say honestly, through two existing capabilities:
  1. `workspace_act` `create_project` with the solution's problem as the description and its
     decision and measure as the project's decision ([workspaces](workspaces.md#a-projects-decision));
  2. `compile_pipeline` with the decision and problem as the intent, the composed methodology the
     person chose (the first by default) and an optional dataset.

  The project then has its first pipeline, built from the methodology; it has no policy, holdout or
  ROI readout. If the pipeline cannot be built, the project is kept and the page says why; Build a
  Pipeline on the project page tries again.

## Errors

| Code | When |
|---|---|
| `invalid_input` | A required field is missing or empty (`/name`, `/problem`, `/decision`, `/measurement/metric`), a field is longer than its bound, a methodology id is not an id or is listed twice (`/methodologies/<n>/id`), a level is outside 1-4, `source` is neither `orama` nor `workspace`, or `update_solution` sends no field to change |
| `forbidden` | `update_solution` or `delete_solution` names one of Orama's solutions |
| `not_found` | No solution by that id in this workspace, or none in Orama's catalogue |
| `unauthorized` | The caller cannot read (lists and reads) or write (changes) the space |
| `workspace_not_ready` | The workspace Zetta is not ready |

## Limits

| Field | At most |
|---|---|
| `name` | 200 bytes |
| `problem`, `decision`, `pipeline` | 4,000 bytes each |
| `measurement.metric`, `baseline`, `target` | 300 bytes each |
| `notes` | 20,000 bytes |
| `methodologies` | 32, each id 1-64 letters, digits, `-`, `_` or `.` |

A workspace's own solutions are not paged: a workspace is expected to hold tens of them.

## How to verify

- `ctest --test-dir build -R 'Solutions\.|SolutionsAreTheWorkspacesOwn'`:
  - `tests/test_solutions.cpp` covers what a solution must carry, a catalogue entry's copy, a
    copy's name and the audit route.
  - `WorkspaceRecordStores.SolutionsAreTheWorkspacesOwnAndOramasStayReadOnly`
    (`tests/test_workspace_records.cpp`, in memory and on PostgreSQL) creates one, lists both
    kinds, reads, edits, refuses editing and deleting Orama's with `forbidden`, duplicates from
    Orama's and from one's own, deletes, refuses another workspace and another space, and lists
    without the catalogue.
- `frontend/tests/solutions.test.ts` covers the page's filter, the methodology field and the
  inputs Start as a Project sends.
- Against a running lake: `create_solution`, then `list_solutions` shows it beside S-1, …;
  `update_solution {id: "S-1", name: "x"}` answers 403 `forbidden`.

## How to change it

- The record and its rules: `include/lake/control/solutions.hpp`, `src/control/solutions.cpp`
  (`WorkspaceSolutionCodec`, schema version 1; a new body field is added there and decoded with a
  default, a breaking change takes version 2 and decodes version 1).
- The capabilities: `src/api/solution_capabilities.cpp`, registered by
  `register_solution_capabilities`.
- The pages: `frontend/src/pages/design/SolutionsPage.tsx`, `SolutionPage.tsx`, `SolutionForm.tsx`
  ([UI](../ui/README.md)).
- If the engine's `create_solution` comes to accept an authored solution, Start as a Project should
  send it there instead of building a pipeline only, and this page and the guide change with it.
