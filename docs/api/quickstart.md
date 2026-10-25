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
| Model | `train_model` (methodology `M-1` and a 0.2 held-out share unless told otherwise), then `expose_model` once its run has succeeded |
| Website key | `create_browser_key` bound to the index (and the model once published) and the website's origins: nested in `quickstart_snippets` when it is sent `origins`, or called by the person or the agent ([browser-keys.md](browser-keys.md)) |

The quickstart's own reads for its bookkeeping (the training run's `get_run`, the model the run made
from `list_models`, the dataset's columns from `get_dataset`) go through `call_workspace`, the path
every engine read takes, under the quickstart call's own authorization and audit, so a status poll
records one audit event rather than one per read.

## Capabilities

Names follow the registry's rule: verb-first actions, noun reads (`Capability::name`). The issue
proposed `quickstart_train`; it is `train_quickstart`, an action, beside `train_model`.

| Capability | Kind | Input | What it does |
|---|---|---|---|
| `start_quickstart` | action, paid | `customer_id`; `workspace_id?`; `quickstart_id?` (resume); `content` with `format` (`csv`, `jsonl`) and `name?`, or `dataset_id` (`<database>/<name>`); `fields?` (the columns to search) | The workspace (the account's first, made when it has none, while `workspace_id` is left out), the dataset, its index, publishing it |
| `quickstart_status` | read | `customer_id`, `workspace_id`, `quickstart_id?` (default: the newest), `include_costs?` | Every step's state, the ids, the training's progress read live, `next`, the workspace's quickstarts; with `include_costs`, `costs` |
| `train_quickstart` | action, paid | `customer_id`, `workspace_id`, `quickstart_id?`, `target?`, `entity_column?`, `methodology?` (default `M-1`), `holdout_fraction?` (default 0.2), `name?` | Starts training; called again once trained, publishes the model; a failed run is trained again by the next call |
| `quickstart_snippets` | action, authorized as a read (POST) | `customer_id`, `workspace_id`, `quickstart_id?`, `origins?` (the website's origins: makes the key now) | `snippets` (`html`, `react`, `curl`, each `{id, title, language, code}`), `key` (`kind: "browser_key"`, `placeholder`, `capabilities`, `create`: the `create_browser_key` call, `secret` and `browser_key` when it made one, `note`), `mcp`, `ready`, `includes_model` |

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

`dataset.columns` is the file's columns in its own order with their types (the index lists its
fields sorted); the console's first guess for training is the last column as the outcome and the
first text column as the row's name.

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
- `QuickstartZetta.ADatasetBecomesSearchAndModelAnswersOnARealEngine` runs the same against a real
  Zetta with `LAKE_TEST_ZETTA_URL` and `LAKE_TEST_ZETTA_TOKEN` set (a local `zetta --headless`): a
  real search through `search_index`, training, publishing and `predict` on the hits.
- By hand over MCP: `tools/call start_quickstart` with a CSV's text, `train_quickstart`,
  `quickstart_status` until `trained`, `train_quickstart` again, `quickstart_snippets` with
  `origins`; the key in the code answers `search_index` and `predict` with that `Origin`, and 403
  with another.
- In a browser: `tools/e2e/quickstart.mjs` ([../deployment/browser-testing.md](../deployment/browser-testing.md)).

## How to change it

- A new step: a field on the record, a `step(...)` in `status_of`, its call in the capability that
  owns it (as a nested registry call), its cost in `costs_for`, and the console's card.
- A new snippet: a function beside `html_snippet` and an entry in `quickstart_snippets`; values go
  in through `script_value`, which never closes a script.
- The website key: what a browser key may do is [browser-keys.md](browser-keys.md#how-to-change-it);
  what the quickstart asks for is `create_input` in `quickstart_snippets`.
