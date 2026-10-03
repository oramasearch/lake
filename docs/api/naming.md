# Names

One name per concept across capabilities, so a value read from one call is passed to the next
under the same name (lake#450).

| Concept | Name | Deprecated aliases still accepted |
|---|---|---|
| A space (customer) | `customer_id` | `id` on `get_customer` |
| A workspace | `workspace_id` | — |
| A run | `run_id` | `run` on `get_run_metrics`, `stop_experiment_trial`, `promote_trial`; `run` inside `workspace_query`/`workspace_act` input |
| A trained model | `model_id` | `model` on `predict`; `model` inside `workspace_query`/`workspace_act` input |
| A search index | `index_id` | `index` on `search_index`; `index` inside `workspace_query`/`workspace_act` input |
| A compute job | `job_id` | `job` inside `workspace_query` input |
| A workspace dataset | `<database>/<name>`, the `id` `list_datasets` answers | `<database>.<name>` and a bare name (in `datasets`), everywhere a dataset or `dataset_path` is read |

`compare_runs` is one concept as well: `names` is optional on the top-level capability and inside
`workspace_query`. Left out, lake reads every metric the runs share (`workspace_query` hands such a
call to the top-level `compare_runs`).

`get_model` and `get_dataset` mean the same thing top-level and inside `workspace_query`. With
`revision` they read a model or dataset **repository** revision; without, they read the workspace's
trained model or dataset, the same record `workspace_query` answers, by the id Ship → Models or
`list_datasets` shows. Their `id` is that id.

`get_repository`, `get_user`, `get_organization`, `get_team` and the other single-entity reads
still name the entity they read `id`; this table covers the names that differed between
capabilities that take the same value.

## How aliases work

A capability lists its deprecated names in `Capability::input_aliases` as `{alias, canonical}`.

- `Registry::add` copies the canonical property into the input schema under the alias, marked
  `"deprecated": true` with a description naming the canonical field, so OpenAPI, MCP `tools/list`
  and `llms-full.txt` show both.
- `Registry::call` and `call_async` move an alias onto its canonical field before validation and
  authorization (`canonical_input`), so the handler, the audit record and every error see only the
  canonical name. Sending both with different values is `invalid_input` at the alias.
- The workspace engine itself takes `run_id` on `get_run`/`get_run_report` and `run` on
  `get_run_metrics`, `model_id` on `get_model`/`model_health` and `model` on
  `expose_model`/`score`/`explain`, and so on. `canonical_workspace_input` accepts either spelling
  in `workspace_query`/`workspace_act` input and sends the one the engine capability takes; errors
  name the field the caller sent.

To rename an input: add the new name to the schema, add `{old, new}` to `input_aliases`, change
the handler to read the new name, and add a row above. Remove an alias only after every caller in
the agent repositories (`zetta-bridge/modules/agent-*`) and the docs has moved.

## Renamed in lake#450

| Capability | Old | New |
|---|---|---|
| `get_customer` | `id` | `customer_id` |
| `predict` | `model` | `model_id` |
| `search_index` | `index` | `index_id` |
| `get_run_metrics`, `stop_experiment_trial`, `promote_trial` | `run` | `run_id` |

Outputs did not change names, except `predict`: each ranked entry's `entity` is now the row's own
key and the row's position is `row` ([workspaces.md](workspaces.md)). `expose_model` and
`expose_index` answer `call` (the lake capability to call) instead of the engine's `endpoint`.
