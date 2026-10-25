# The quickstart process

lake#1128. Steve, 2026-10-24: *"it's meant to get new users having success early"*.

## What it is

A new person drops a dataset and Lake takes it to search and model answers on their own website:

1. **Dataset.** The file goes into a workspace as a dataset. A person with no workspace gets one.
2. **Search index.** Lake builds a search index over the dataset and publishes it.
3. **Search on your website.** Code for a plain web page and for React, with Lake's host and the
   ids filled in, and the key the code needs.
4. **Model.** One call trains a model on the same dataset.
5. **Model answers on your website.** Once trained, the model is published and the code shows the
   model's answer beside each search result.

Each step has a state (pending, running, done, failed with why). Calling again resumes from the
step that has not finished. It is the console's home page ([../ui/quickstart.md](../ui/quickstart.md))
and four capabilities in the one registry, so REST, OpenAPI, MCP, `llms.txt`, `llms-full.txt` and
the Lake skill all carry it. An agent on lake.live finds it first: the Quickstart group and flow
come before every other, under a *Start Here* section.

## Why it composes, and does not reimplement

The steps are capabilities Lake already has. The quickstart calls them through the registry as the
same person (`Registry::call_async` on a copy of the context, `nested` in
`src/api/quickstart_capabilities.cpp`). So permissions, the balance gate, audit and usage metering
are each one's own, unchanged:

| Step | Called through the registry |
|---|---|
| Workspace (when none is named) | `start_workspace` |
| Dataset | `upload_dataset` with `if_exists: "rename"` (so its CSV check and name rule apply), or an existing `dataset_id` checked with the engine's `get_dataset` |
| Search index | `create_index`, then `expose_index` |
| Model | `train_model` (methodology `M-1` and a 0.2 held-out share unless told otherwise), on the dataset or, when it cannot train as it is, on a training copy made with `upload_dataset` ([columns](#columns)); then `expose_model` once its run has succeeded |
| Website key | `create_browser_key` bound to the index (and the model once published) and the website's origins: nested in `quickstart_snippets` when it is sent `origins`, or called by the person or the agent ([browser-keys.md](browser-keys.md)) |

The quickstart's own reads for its bookkeeping (the training run's `get_run`, the model the run made
from `list_models`, the dataset's columns from `get_dataset`, their kinds from `analyze_dataset`, the
rows of a training copy through the engine's `sql`) go through `call_workspace`, the path
every engine read takes, under the quickstart call's own authorization and audit, so a status poll
records one audit event rather than one per read.

## Capabilities

Names follow the registry's rule: verb-first actions, noun reads (`Capability::name`). The issue
proposed `quickstart_train`; it is `train_quickstart`, an action, beside `train_model`.

| Capability | Kind | Input | What it does |
|---|---|---|---|
| `start_quickstart` | action, paid | `customer_id`; `workspace_id?`; `quickstart_id?` (resume); `content` with `format` (`csv`, `jsonl`) and `name?`, or `dataset_id` (`<database>/<name>`); `fields?` (the columns to search) | The workspace (the account's first, made when it has none, while `workspace_id` is left out), the dataset, its index, publishing it |
| `quickstart_status` | read | `customer_id`, `workspace_id`, `quickstart_id?` (default: the newest), `include_costs?` | Every step's state, the ids, the training's progress read live, `next`, the workspace's quickstarts; with `include_costs`, `costs` |
| `train_quickstart` | action, paid | `customer_id`, `workspace_id`, `quickstart_id?`, `target?` (default: `suggested`), `target_value?` (the value of a category to look for), `entity_column?`, `as_of?` (the snapshot date), `methodology?` (default `M-1`), `holdout_fraction?` (default 0.2), `name?` | Prepares the columns ([columns](#columns)) and starts training; called again once trained, publishes the model; a failed run is trained again by the next call |
| `quickstart_snippets` | action, authorized as a read (POST) | `customer_id`, `workspace_id`, `quickstart_id?`, `origins?` (the website's origins: makes the key now) | `snippets` (`html`, `react`, `curl`, each `{id, title, language, code}`), `prompt` ([the agent prompt](#the-agent-prompt)), `key` (`kind: "browser_key"`, `placeholder`, `capabilities`, `create`: the `create_browser_key` call, `secret` and `browser_key` when it made one, `note`), `mcp`, `ready`, `includes_model` |

`start_quickstart` takes up to 64 MiB of JSON, as `upload_dataset` does; the engine takes at most
16 MiB of content.

### The answer

`start_quickstart`, `quickstart_status` and `train_quickstart` answer one shape:

```json
{
  "quickstart_id": "uuid", "customer_id": "…", "workspace_id": "…",
  "state": "search_ready",
  "next": "Search works: quickstart_snippets answers the website code. …",
  "steps": [
    {"step": "workspace", "title": "Workspace", "state": "done", "error": null},
    {"step": "dataset", "title": "Dataset", "state": "done", "error": null},
    {"step": "search_index", "title": "Search Index", "state": "done", "error": null},
    {"step": "website", "title": "Search on Your Website", "state": "done", "error": null},
    {"step": "model", "title": "Model", "state": "pending", "error": null},
    {"step": "model_answers", "title": "Model Answers on Your Website", "state": "pending", "error": null}
  ],
  "dataset": {"id": "datasets/movies", "database": "datasets", "name": "movies", "rows": 40,
              "columns": [{"name": "title", "type": "string"}, …], "source": "upload"},
  "index": {"id": "uuid", "fields": ["description", "genre", …], "published": true},
  "training": null, "model": null,
  "search": {"capability": "search_index", "input": {"customer_id": "…", "workspace_id": "…", "index_id": "…", "query": ""}},
  "predict": null
}
```

`state` is one of `waiting_for_workspace`, `not_started`, `in_progress`, `search_ready`, `training`,
`trained`, `done` or `failed`. A failed step carries `error: {code, message}`, the refusal of the
capability it called, word for word; `next` names the call that tries it again. The `model` step
carries `progress` (`status` from `get_run`, `started_at`) while it trains.

`dataset.columns` is the file's columns in its own order (the index lists its fields sorted), each
with its storage `type` and, from lake#1181, its detected `kind` ([columns](#columns)).
`snapshot_date` and `suggested` (`{target, target_value, entity_column}`) are on every answer.

## Columns

lake#1181. Steve, 2026-10-24, after the Quickstart on lake-dev (v0.7.0) refused a dataset with a date
column (`transform_required: add_snapshot_date`) and showed the raw code: *"it should be able to use
any type of columns (auto-detect)"*.

**Detection.** `start_quickstart` asks the engine's own column profile, `analyze_dataset`
(`sample_rows` 10000, `as_of` the snapshot date), and keeps each column's kind on the record
(`detect_columns` in `src/api/quickstart_columns.cpp`; Lake writes no profiler of its own). Each entry
of `dataset.columns`:

```json
{"name": "Country", "type": "string", "kind": "categorical", "title": "category, 4 values",
 "distinct_count": 4, "null_rate": 0, "values": ["Canada", "France", "Japan", "Brazil"],
 "counts": [20, 15, 15, 10], "identifier": false,
 "feature": {"use": true, "reason": "a category of 4 values"},
 "predict": {"can": true, "choose_value": true, "values": ["Canada", "France", "Japan", "Brazil"]}}
```

| `kind` | From the engine's `semantic_type` | Learned from | Can be predicted |
|---|---|---|---|
| `numeric` | numeric | yes | no (many values) |
| `categorical` | categorical | up to 32 values | 3 to 20 values, with `target_value` |
| `boolean` | boolean, or any column with exactly two values (free/pro, 0/1) | yes | yes |
| `datetime` | datetime | no: left out | no |
| `free_text` | free_text | no: needs a text methodology | no |
| `identifier` | identifier | no: names a row | no |
| `empty` | no values | no | no |
| `constant` | one value | no | no |
| `unsupported` | json, unknown | no | no |

The rules are the default methodology's, M-1: gradient-boosted trees answering a yes-or-no question
from numbers and categories (zetta `docs/data/featurization.md`). Another `methodology` trains as it
did before, without preparation. A quickstart made before lake#1181 is detected live by
`quickstart_status` and kept by the next `train_quickstart`.

**What is offered first** (`suggested`): a two-valued column whose name says it is an outcome
(`churned`, `is_active`, `popular`…), else the last two-valued column, else the category with the
fewest values and its most common value; the first identifier names a row. The console lists only
columns with `predict.can`, each with its `title`.

**Snapshot date.** `snapshot_date` is when the dataset was uploaded (or when the quickstart took an
existing one), kept on the quickstart record in the workspace Zetta. `train_quickstart`'s `as_of`
(`2026-10-24` or an ISO-8601 time) changes it; the console's date field sends it. It is declared to
the engine as `analyze_dataset`'s `as_of`, with the column to predict as its `label`, so the engine's
leakage checks run: a column it flags is left out ("it may give the answer away"). **Date and time
columns are left out of the first model**: zetta v0.5.0's `train_model` takes no `as_of` (only the
`run` capability's dataset contract does), and the website's model answers come from the rows search
returns, so a feature the rows do not hold could not be answered. Once `train_model` takes one,
dates can become features measured from the snapshot date
([zetta#1494](https://github.com/oramasearch/zetta/issues/1494)).

**The training copy.** When the dataset trains as it is (every input usable or left out by the
engine itself, the column to predict already 0/1, a unique column naming the rows), `train_model`
gets it unchanged (`preparation.copy: false`). Otherwise Lake reads the rows through the engine's
`sql` as the person (`SELECT COUNT(*)`, then 5,000 rows a page, at most 1,000,000), writes the usable
inputs, the column that names a row and the column to predict as 0/1 (1 where it equals the value to
look for: `target_value`, else a yes-like value, else the rarer of two) as JSON Lines (CSV when that
would pass upload_dataset's 16 MiB), loads it with a nested `upload_dataset` named
`<dataset>_training` (`if_exists: "rename"`), and trains on it. Upload, train and their metering,
balance gate and audit are each capability's own. The copy is a dataset in the workspace like any
other; the website's rows still predict, because the model reads only columns they hold.

A column that names a row must be unique: the engine keys rows by it, and a repeating name (a first
name) merges rows and their answers (on v0.5.0 the run fails "training labels are a single class").
So when the chosen column repeats, or none is chosen, Lake numbers the rows (`row_number`) and keeps
the chosen column as an input; `preparation.note` says so.

`training.preparation` records it: `{methodology, copy, dataset_id, format?, entity, used, dropped:
[{name, reason}], label: {column, positive, rows_yes, rows_no}, rows, skipped, note, applied}`.

**Refusals in plain words.** A choice that cannot train is refused before anything runs: one sent
(`target`, `target_value`, `entity_column`) is `invalid_input` with its path and a plain message; one
Lake chose is kept on the model step as `choose_columns`. A copy with no row to learn from (no value,
the value appears in no row, or every row has it) is `cannot_prepare`. When `train_model` answers
`transform_required: <name>: …`:

| Refusal | What Lake does |
|---|---|
| `add_snapshot_date` | Leaves out every detected date column and any column whose values are ISO dates or times (95%, the engine's rule), and trains once more |
| `add_entity_key`, `capture_label` | Numbers the rows and writes the target as 0/1, and trains once more |
| anything else, or the same again | Keeps it on the model step as `training_refused` with a plain message saying what to choose, and `transform` naming it for an agent; `next` repeats it |

A failed run's reason (`get_run`'s `error`, or its last error log line) is explained the same way
(`run_failed`, with the engine's words in `detail`). The page never shows the raw code, and its
button says *Train with these choices*.

### A workspace that is starting

Without `workspace_id`, `start_workspace` answers the account's first workspace, made when it has
none. A new workspace's Zetta takes time to start, and the quickstart's record lives in that Zetta,
so `start_quickstart` answers `state: "waiting_for_workspace"` and keeps nothing: call it again with
the same input and the `workspace_id` once `quickstart_status` reports `workspace.ready`. The
console holds the dropped file and does that itself.

### The balance

`start_quickstart` and `train_quickstart` are on `paid_work_capabilities()`
([billing.md](billing.md#the-gate)), so a blocked account is refused before any step starts:
`402 payment_required` with `add_funds_url` in `error.details`, never a failed step. The nested
calls are gated as well, and a nested `payment_required` is answered as it is and changes nothing.
`quickstart_status` with `include_costs` answers `costs`:

```json
{"billing": true, "currency": "USD",
 "balance": {"state": "blocked", "balance_micros": 0, "add_micros": 20000000, "add_funds_url": "…"},
 "steps": [{"step": "model", "capabilities": ["train_model", "expose_model"],
            "billed_by": [{"dimension": "cpu", "title": "CPU", "unit": "mcpu_seconds",
                           "rate": {"unit_micros": 2000, "per": 60000}}, …],
            "how": [{"capability": "train_model", "how": "training in the engine; the model, on its volume"}, …],
            "note": "training runs on the workspace engine for as long as it takes, …"}, …]}
```

The dimensions come from `capability_meters()` (`src/api/usage_recorder.cpp`), the rates from the
price list in force (`price_list`) and the balance from `account_balance`, each read through the
registry, only where billing is on. A step's amount depends on how long its work runs, so the
rates are given, not a total.

## The website key

The code goes into a public web page, so its key is a **browser key** (lake#1152,
[browser-keys.md](browser-keys.md)): bound to the quickstart's index (and its model once
published) in this workspace, callable only for `search_index` and `predict` on them, only from the
website origins it lists (Lake checks `Origin`, and its CORS answer names only those origins), at
most 600 calls a minute, and lasting until it is revoked. It belongs to the workspace, not to the
sign-in that made it, so signing out leaves the website working.

- **With `origins`** (`["https://www.example.com"]`), `quickstart_snippets` makes the key through a
  nested `create_browser_key` call as the same caller (its authorization, write on the account, and
  its central audit are its own), answers its secret once in `key.secret` with the key's record in
  `key.browser_key`, and puts the secret in the code where it says `YOUR_LAKE_SEARCH_KEY`. Every
  call with `origins` makes a new key; the earlier ones keep working until revoked. A refusal (an
  origin that is not `https://host[:port]`, or `http://` other than localhost) is
  `create_browser_key`'s, word for word, and nothing is made. The console sends the address the
  person typed.
- **Without `origins`** it makes nothing: the code carries `YOUR_LAKE_SEARCH_KEY`, and `key.create`
  is the `create_browser_key` call with the account, workspace, index (and model) filled in and
  `origins: ["https://www.example.com"]` to replace. Changing nothing, it is authorized as a read; it
  is POSTed because with `origins` it writes.
- The curl snippet sends `Origin` with the key's first origin (or `https://www.example.com`), since a
  browser key answers only calls that name one of its origins.
- A key made before the model was published binds no model and can only search: once the model is
  published, `key.capabilities` lists `predict`, which that key lacks, and the console offers a new
  key.
- An agent on an API token, or an MCP client, can make the key: `create_browser_key` needs write on
  the account, not a signed-in session (`create_token` still refuses a token credential).
- Keys are listed and revoked with `list_browser_keys` and `revoke_browser_key`, and in the console
  on the index's page (Ship → Search Index, *Website Keys*). Every search and answer is billed to the
  account. The quickstart never stores a key: the secret is answered once, and the control plane
  keeps only its hash.

## The agent prompt

lake#1185. Steve, 2026-10-24: *"you should have a tab for copy agent prompt (default), and another tab
for just the source code... but the copy prompt should include the source and the full context for
the agent to success in installing it into their website"*.

`quickstart_snippets` answers `prompt`, one Markdown block a person pastes into a coding agent in
their website's project (`agent_prompt` in `src/api/quickstart_capabilities.cpp`). It holds:

- the goal: a search box over the index, with the model's answer beside each result when a model is
  published (*how likely `<column>` is `<value>`*, from `training.preparation.label`);
- what to call: Lake's origin, `GET /v1/search_index` with the account, workspace and index ids,
  `POST /v1/predict` with the model id when there is one, the `Authorization: Bearer` header, and the
  index's fields;
- the key: already in it when this call made it with `origins`, otherwise `YOUR_LAKE_SEARCH_KEY` with
  the `create_browser_key` input and where to make one in the console; what it can and cannot do
  (only this index and model, changes nothing, only the listed origins, its calls a minute, billed to
  the account, revocable) and that it belongs in client code;
- the allowed origins (the key's, when made) and what to do on another origin (a new key; never a
  workaround for the CORS refusal);
- the answers: the `{ok, value}` / `{ok: false, error}` envelope, `search_index`'s `hits` (`id`,
  `score`, `fields`) and `predict`'s `ranked` (`row`, `risk`);
- how to build it: find the right place, use the site's framework, components and styles, debounce,
  show fields not JSON, handle nothing typed, searching, no results, errors and `429 rate_limited`;
- the source: the plain web page and the React component, as in `snippets`;
- how to check it: a curl search with `Origin` and `-D -`, the `Access-Control-Allow-Origin` header,
  and results (and answers) in the browser;
- Lake's MCP server (`/mcp`) and `llms.txt`.

The secret is in the prompt only when the same call made the key; the console puts in the key it
holds, the same way it does for the code. The console shows the prompt first, in the *Copy agent
prompt* tab, and the snippets in *Source code* ([../ui/quickstart.md](../ui/quickstart.md)).

## Where it lives

Everything is in the user's workspace Zetta: the dataset, the index and the model are the engine's,
and the quickstart's state is one sealed record per quickstart in the collection `lake_quickstarts`
(found by the account, newest last; [workspace records](../architecture/workspace-records.md)). Its
body holds the dataset, index, training settings and run id, model id, and each step's last error.
Nothing of the quickstart's own is added to lake's PostgreSQL control plane: the workspace a person
gets is the one `start_workspace` makes, and the website key is a browser key (`browser_keys`,
decision 53), both of which multitenancy already holds. Audit goes to the workspace's own chain
(`audit_routing.cpp`).

## Verify

- `ctest --test-dir build -R Quickstart` runs `Quickstart.*` (`tests/test_quickstart.cpp`) over a
  test double of the workspace engine: the whole process (with the browser key made through
  `quickstart_snippets` with `origins`, searching from that origin), a failed step kept and resumed, a blocked
  balance refused with the top-up link and nothing started, another account's workspace refused,
  a person with no workspace waiting for one, input refusals, and the OpenAPI, MCP, `llms.txt`,
  `llms-full.txt` and skill listings.
- `Quickstart.ADroppedDatasetBecomesSearchThenModelAnswers` checks the agent prompt's contents
  search-only without a key, with the key a call made (its secret and origin in it), and with the
  model published (`predict`, its id and answer shape).
- `QuickstartColumns.*` (`tests/test_quickstart_columns.cpp`) covers the column rules: every kind,
  what is offered first, the choice checks, the training copy (numbered rows, the 0/1 answer, CSV),
  dates found in values, and every refusal explained without its code.
- `Quickstart.AnyColumnTypesTrainADatedDatasetIsPreparedNotRefused`,
  `ATransformRequiredLakeCanSatisfyIsAppliedAndTriedOnce` and `WhatCannotBeTrainedIsSaidInPlainWords`
  run Steve's shape (first name, country, a date, an id, free text, numbers, an empty and a constant
  column) through the test double, including a `transform_required` retry.
- `QuickstartZetta.ADatedDatasetWithAnyColumnTypesTrainsAndAnswersOnARealEngine` trains that shape
  on a real Zetta (v0.5.0), publishes it and predicts on the search hits' rows, date included.
- `QuickstartZetta.ADatasetBecomesSearchAndModelAnswersOnARealEngine` runs the same against a real
  Zetta with `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` set (a local `zetta --headless`): a
  real search through `search_index`, training, publishing and `predict` on the hits.
- By hand over MCP: `tools/call start_quickstart` with a CSV's text, `train_quickstart`,
  `quickstart_status` until `trained`, `train_quickstart` again, `quickstart_snippets` with
  `origins`; the key in the code answers `search_index` and `predict` with that `Origin`, and 403
  with another.
- In a browser: `tools/e2e/quickstart.mjs` ([../deployment/browser-testing.md](../deployment/browser-testing.md)).

## How to change it

- What a kind is used for: `describe` in `src/api/quickstart_columns.cpp`; what is offered first:
  `suggest`; a refusal Lake fixes: `fix_for`, and its words: `explain_refusal`.

- A new step: a field on the record, a `step(...)` in `status_of`, its call in the capability that
  owns it (as a nested registry call), its cost in `costs_for`, and the console's card.
- A new snippet: a function beside `html_snippet` and an entry in `quickstart_snippets`; values go
  in through `script_value`, which never closes a script.
- The website key: what a browser key may do is [browser-keys.md](browser-keys.md#how-to-change-it);
  what the quickstart asks for is `create_input` in `quickstart_snippets`.
