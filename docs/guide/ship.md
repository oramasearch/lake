# Models and Search Indexes

Ship is where what you trained and indexed leaves Lake. This page covers the two things your own
code calls: **published models**, which score your records, and **published search indexes**,
which answer full-text searches. Both run in your workspace; Lake checks the key, the
workspace and that the model or index is published, and records every call.

Every call on this page names two ids. `customer_id` is the id of the account that owns your
workspaces (yours, or your organization's), and `workspace_id` is the workspace, as in the page
address `/w/<workspace id>/…`. [Getting Started](/docs) says where to find both.

## Concepts

| Term | What it is |
|---|---|
| **Model** | A learned function that makes a prediction. Each training of it is a **version** with its own model id. **Train → Models** is about training it: its versions, held-out scores and what it learned from ([Train a Model](/docs/train)). **Ship → Models** is about serving it: calling it, who may, where its answers go and how it is doing. A pipeline can train several models. A model uploaded on its own lives as a model repository, with no pipeline ([Repositories](/docs/repositories)). |
| **Predict key** | A Lake API token whose only scope is `predict`: it can call published models and do nothing else. |
| **Published** | A model or index that answers calls from outside Lake. Anything not published can still be used inside Lake, but `predict` and `search_index` refuse it. |
| **Promoted** | The model an experiment chose as its best trial ([Experiments](/docs/experiments)). It is marked **promoted** on Train → Models and Ship → Models. Promoting a version to Ship from **Train → Models** publishes it. |
| **Search index** | A full-text (BM25F) index over one dataset's columns. |
| **Search key** | A Lake API token whose only scope is `search_index`: it can search published indexes and do nothing else. |
| **Lake key** | Any Lake API token, made on Developer → API or Developer → MCP ([Tokens and Keys](/docs/tokens)). |

## Ship → Models

**Ship → Models** serves every model in the workspace, one row per model with its versions grouped
as on Train → Models. The figures on top count the published models, and every model's calls,
errors and mean latency over the last 24 hours. Each row shows the version serving and its model
id, its calls, error rate and latency, and **Publish**, **Call** and **Unpublish**. The model's name
opens its serving page; **training** under it opens Train → Models.

- **Search models…** filters by name, pipeline, project or id.
- **Published** and **Not published** filter by state.
- **Train → Models** in the header opens the training side.

### Publish a Model

1. Open **Ship → Models**.
2. Press **Publish v2** on the model's row (the button names its latest version). The row then
   shows **Call** and **Unpublish**. To publish another version, promote it from Train → Models.

- REST: `POST /v1/expose_model`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>"}
```

- MCP: `expose_model` with the same input.

The answer's `call` names the Lake capability that now serves the model (`predict`), with its method,
path and input filled in.

### Call a Published Model

Press **Call** on a published model's row. The **Call a Model from Your Code** drawer shows a `curl`
command with your account, workspace and model filled in and one row keyed by the model's
features, ready to paste. Opening it from the page header shows every published model at once.

```
curl -s https://lake.example.com/v1/predict \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>", "rows": [{"entity": "acme", "support_tickets": 11, "tenure_months": 4}]}'
```

- REST: `POST /v1/predict` with `customer_id`, `workspace_id`, `model_id` and `rows`.
- MCP: the tool `predict`, with the same input.

What you send:

| Field | Required | Meaning |
|---|---|---|
| `model_id` | yes | The model's id, as Ship → Models shows it (`model` is an older name for it and still works). |
| `rows` | yes | One or more records, each keyed by the model's feature columns. |
| `entity_column` | no | The field in each record that carries its own key. Default `entity`. |
| `top_k` | no | How many ranked rows to answer (0 or more). |

What comes back: the rows ranked by risk. Each ranked entry has `entity` (the value of the record's
`entity_column` field, left out when the record has none), `row` (its position in the `rows` you
sent) and `risk`, and the answer says what drives the scores. A record with a missing feature is
refused with its place, counted from one and named by its key: the third record, keyed `partial`,
reads *row 3 ('partial') has no 'tenure'* and is answered `invalid_input` at `/rows/2/tenure`
(the path is the record's position in `rows`, from 0).

To ask for only the top five of a batch, keyed by your own column:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>", "entity_column": "account_id", "top_k": 5, "rows": [{"account_id": "a-17", "support_tickets": 11, "tenure_months": 4}, {"account_id": "a-42", "support_tickets": 0, "tenure_months": 30}]}
```

`predict` is a `POST` because it carries your records, but it changes nothing: it needs `read` on the
account, so a read-only key can call it.

### Unpublish a Model

Press **Unpublish** on the model's row and confirm. Unpublishing a model stops every outside call
at once; you can publish it again later.

- REST: `POST /v1/unexpose_model`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>"}
```

- MCP: `unexpose_model` with the same input.

### A Model's Serving Page

Press a model's name on **Ship → Models**. Its page has four views; **Training** in the header opens
the same model on Train → Models.

- **Inference**: the endpoint (`POST /v1/predict`, and the MCP tool `predict`), the model id, the
  call to paste, and **Try It**: edit the rows (prefilled with one row keyed by the model's
  features) and press **Predict** to see the rows ranked by risk and the whole answer. Try It calls
  `predict` as you, so the version must be published.
- **Access**: who may call it. A call needs a Lake key of someone with `read` on the workspace; the
  list shows your keys scoped to `predict` (or to everything). **New predict key** makes a token
  whose only scope is `predict` and shows its secret once. A call carrying a key is answered from
  any origin; a browser signed in to Lake is refused when the calling page is not Lake's own. Lake
  keeps no per-model list of origins.
- **Destinations**: where its answers go. Each answer goes back to the caller only; no app, topic
  or webhook receives a model's answers yet. An app calls a model as any code does.
- **Monitoring**: calls, errors (refused and failed), mean latency and the workspace's `predict`
  requests over 1 hour, 24 hours, 7 days or 30 days, as figures and charts, and the recent minutes
  with calls. Lake counts calls by the minute in your workspace and keeps no row a caller sent.
  Prediction drift against the training data is not measured yet.

**Publish** and **Unpublish** are in the side column, beside the model's facts.

### Watch a Model from Code

- REST: `GET /v1/model_traffic?customer_id=<customer id>&workspace_id=<workspace id>&model_id=<model id>`
- MCP: the tool `model_traffic`, with the same input.

Without `model_id` it answers every model with calls. `from_ms`, `to_ms` and `step_ms` (a whole
number of minutes) choose the range and its steps; the default is the last 24 hours in about 48
steps. Each model answers `calls`, `errors`, `client_errors`, `server_errors`, `mean_ms` and its
`steps`; `requests` counts the workspace's `predict` requests by status class, and `drift` says it
is not measured yet.

### List Models from Code

- REST: `POST /v1/list_models`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>"}
```

- MCP: `list_models` with the same input. Each model carries its id, name, project and
  pipeline, run, score, feature schema, creation time and whether it is published.

## Ship → Search Indexes

**Ship → Search Indexes** lists the workspace's indexes with the dataset each covers and its fields.
The figures count the indexes, the datasets they cover and the fields indexed. **Search indexes…**
filters the list; open an index to search it, publish it and call it from your own code.

### Create an Index

1. Open **Ship → Search Indexes** and press **New index**.
2. Choose the **Dataset**. Run outputs are listed apart, under *Results*.
3. Under **Fields to search, comma separated**, name the text columns to index, or leave it empty
   to index every column.
4. Press **Create index**. The new index opens.

- REST: `POST /v1/create_index`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "table": "support_tickets", "fields": ["subject", "body"]}
```

- MCP: `create_index` with the same input. The answer carries the new index's id.

### Try a Search

The index page's **Try a Search** card is a playground. Type under **Search for**, choose a field
under **In** (or keep *all fields*), set **Hits** (1 to 100) and press **Search**. Each hit shows the
record's id and its score. The query, field and hit count stay in the page address, so a search can
be shared. Searching here works whether or not the index is published.

- REST: `POST /v1/search_workspace_index`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "target": "<index id>", "query": "renewal risk", "top_k": 10}
```

- MCP: `search_workspace_index` with the same input.

### Publish an Index

On the index page, the **Publishing** card says whether the index answers outside calls. Press
**Publish**. To stop, press **Unpublish** and confirm: every outside call stops at once, and
searching in Lake keeps working.

- REST: `POST /v1/expose_index`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "index_id": "<index id>"}
```

- The same with `unexpose_index` unpublishes it. MCP: `expose_index` with the same input.

### Create a Search Key

Once the index is published, the **Search Key** card offers **Create a search key**. The key's only
scope is `search_index`, so it can search published indexes and nothing else. It is shown once:
copy it then. It keeps working for your other published indexes until you revoke it on **Tokens**,
and expires no later than the sign-in that made it would have, at most 30 days; signing out does
not end it.

- REST: `POST /v1/create_token`:

```json
{"name": "search support_tickets", "scopes": ["search_index"]}
```

- MCP: `create_token` with the same input. The answer's `secret` is the key.

### Search from Your Code

The **Call It from Your Code** card shows the call in curl, JavaScript and Python with your account,
workspace and index filled in:

```
curl -s -H "Authorization: Bearer $LAKE_SEARCH_KEY" \
  "https://lake.example.com/v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index_id=<index id>&query=renewal%20risk&top_k=10"
```

- REST: `GET /v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index_id=<index id>&query=renewal%20risk`
- MCP: the tool `search_index`, with the same inputs.

| Field | Required | Meaning |
|---|---|---|
| `index_id` | yes | The index's id (`index` is an older name for it and still works). |
| `query` | yes | The text to search for. |
| `field` | no | Limit the search to one indexed field. |
| `top_k` | no | How many hits, 1 to 100 (default 10). |
| `fields` | no | Which of the index's fields each hit carries (default: all of them). `[]` (over REST, `fields=` with no value) returns ids and scores only. A field the index does not hold is refused, and the message lists the index's fields. |

The answer's `hits` are `{id, score, fields}`, best first. `fields` holds the matched row's values,
which is what you show in your search box; `id` is the workspace's internal row id, not a column of
your table. Lake checks the key, checks that the index is published, runs the search in your
workspace and records the query.

For an index over `account_id` and `plan`, each hit's `fields` holds that row's `account_id` and
`plan`. Ask for some fields only with `&fields=plan` (repeat it, or comma-separate, for more).

## Delete a Model Version or a Search Index

- On Ship → Model, press **Delete**, type the model's name and press **Delete model version**. That
  version's trained model goes, and stops answering if it was published; its other versions and
  the run that trained it stay. REST `POST /v1/delete_model`, MCP `delete_model`.
- On Ship → Search Index, press **Delete**, type the indexed table's name and press **Delete
  index**. The index goes, and stops answering if it was published; the dataset stays. REST
  `POST /v1/delete_index`, MCP `delete_index`.

Both take `customer_id`, `workspace_id` and `id`, and `"dry_run": true` to check first. Neither can
be undone.

## Limits

- Only a **published** model or index answers `predict` or `search_index`. Publishing is per model
  and per index.
- A model or index is reached only through Lake (`/v1/predict`, `/v1/search_index` or the MCP tools
  of the same names), never at your workspace's own address.
- `search_index` answers at most 100 hits per call.
- A model's calls are counted by the minute and kept 30 days; `model_traffic` reads at most 31 days
  and 10,000 steps at once. Counts can lag by up to ten seconds, and a Lake that stops abruptly can
  lose its last ten seconds of counts.
- Workspace reads time out after 15 seconds and actions after 60.
- A key expires no later than the sign-in that made it would have, at most 30 days, unless your
  account allows keys that never expire ([Tokens and Keys](/docs/tokens)). Signing out does not
  end it.

## Permissions

| Step | Needs |
|---|---|
| See models and indexes, their traffic (`model_traffic`), search in the playground | `read` on the account that owns the workspace |
| Call `predict` or `search_index` | `read` on the account, and a key whose scopes include the capability |
| Publish, unpublish, create an index, create a key | `write` on the account |

## Troubleshooting

| You see | What it means | What to do |
|---|---|---|
| `unauthenticated` (401) | No key, or the key is unknown, revoked or expired; the error's `reason` says which. | Send `Authorization: Bearer <key>`; make a new key on Developer → API. |
| `unauthorized` (403) | The key is valid but does not grant this call, such as a search key calling `predict`. | Use a key whose scopes include the capability. |
| `invalid_input` at `/rows/2/tenure` | The third row (*row 3* in the message; the path counts from 0) lacks the feature `tenure`. | Send every feature the model reads; the Call drawer lists them. |
| `invalid_input` at `/workspace_id` or `/customer_id` | The workspace is unknown, or is not one of that account's workspaces. | Copy both ids from the page address or the Call drawer. |
| A refusal saying the model or index is not published | It was never published, or was unpublished. | Publish it on Ship → Models or the index page. |
| `workspace_not_ready` (503) | The workspace is starting or stopped. | Open any Design, Train or Ship page and wait until it is ready, or start it ([Workspaces](/docs/workspaces)). |
| `not_found` | No model or index with that id in this workspace. | Check the id and the workspace; ids are per workspace. |

## Related Pages

- [Train a Model](/docs/train): where models come from, their versions, and the held-out score.
- [Experiments](/docs/experiments): experiments, trials and promotion.
- [Datasets and Loading Data](/docs/load-data): the datasets an index covers.
- [Apps](/docs/apps): ship a whole data app instead of an endpoint.
- [Tokens and Keys](/docs/tokens): make and revoke keys.
- [REST API and OpenAPI](/docs/developer) and [MCP Setup](/docs/mcp): calling Lake from code and agents.
