# Models and Search Indexes

Ship is where what you trained and indexed leaves lake. This page covers the two things your own
code calls: **published models**, which score your records, and **published search indexes**,
which answer full-text searches. Both run in your workspace's own Zetta; lake checks the key, the
workspace and that the model or index is published, and records every call.

Every call on this page names two ids. `customer_id` is the id of the account that owns your
workspaces (yours, or your organization's), and `workspace_id` is the workspace, as in the page
address `/w/<workspace id>/…`. [Getting Started](/docs) says where to find both.

## Concepts

| Term | What it is |
|---|---|
| **Model** | A learned function that makes a prediction. **Ship → Models** lists the models your workspace trained: each one's name, the project, pipeline and run it came from, its score on held-out rows, the features it reads and when it was trained. A pipeline can train several models. A model uploaded on its own lives as a model repository, with no pipeline ([Repositories](/docs/repositories)). |
| **Published** | A model or index that answers calls from outside lake. Anything not published can still be used inside lake, but `predict` and `search_index` refuse it. |
| **Promoted** | The model a study chose as its best trial ([Experiments](/docs/experiments)). It is marked **promoted** on Ship → Models. |
| **Search index** | A full-text (BM25F) index over one dataset's columns. |
| **Search key** | A lake API token whose only scope is `search_index`: it can search published indexes and do nothing else. |
| **Lake key** | Any lake API token, made on Developer → API or Developer → MCP ([Tokens and Keys](/docs/tokens)). |

## Ship → Models

**Ship → Models** lists every model in the workspace. The figures on top count the models (and how
many have weights), the published ones, the ones with a held-out score and the ones a study
promoted. The table shows each model's name, its project and pipeline, when it was trained, its
score and whether it is published. A model trained by a study names its study and trial, and the
model a study promoted carries a **promoted** chip.

- **Search models…** filters by name.
- **Published** and **Not published** filter by state; when the workspace has several kinds of
  model, a second row of chips filters by kind.
- **Runs** in the header opens Train → Runs, where every model came from.

### Publish a Model

1. Open **Ship → Models**.
2. Press **Publish** on the model's row. The row then shows **Call** and **Unpublish**.

- REST: `POST /v1/workspace_act` with the engine's `expose_model`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "expose_model", "input": {"model_id": "<model id>"}}
```

- MCP: `workspace_act` with the same input.

The answer's `call` names the lake capability that now serves the model (`predict`), with its method,
path and input filled in.

### Call a Published Model

Press **Call** on a published model's row. The **Call a Model from Your Code** drawer shows a `curl`
command with your account, workspace and model filled in and one row keyed by the model's
features, ready to paste. Opening it from the page header shows every published model at once.

```
curl -s https://lake.oramadev.net/v1/predict \
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
refused with its place: *row 2 has no 'tenure'* is answered `invalid_input` at `/rows/2/tenure`.

To ask for only the top five of a batch, keyed by your own column:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>", "entity_column": "account_id", "top_k": 5, "rows": [{"account_id": "a-17", "support_tickets": 11, "tenure_months": 4}, {"account_id": "a-42", "support_tickets": 0, "tenure_months": 30}]}
```

`predict` is a `POST` because it carries your records, but it changes nothing: it needs `read` on the
account, so a read-only key can call it.

### Unpublish a Model

Press **Unpublish** on the model's row and confirm. Unpublishing a model stops every outside call
at once; you can publish it again later.

- REST: `POST /v1/workspace_act`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "unexpose_model", "input": {"model_id": "<model id>"}}
```

- MCP: `workspace_act` with the same input.

### List Models from Code

- REST: `POST /v1/workspace_query`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "list_models", "input": {}}
```

- MCP: `workspace_query` with the same input. Each model carries its id, name, project and
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

- REST: `POST /v1/workspace_act` with the engine's `create_index`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "create_index", "input": {"table": "support_tickets", "fields": ["subject", "body"]}}
```

- MCP: `workspace_act` with the same input. The answer carries the new index's id.

### Try a Search

The index page's **Try a Search** card is a playground. Type under **Search for**, choose a field
under **In** (or keep *all fields*), set **Hits** (1 to 100) and press **Search**. Each hit shows the
record's id and its score. The query, field and hit count stay in the page address, so a search can
be shared. Searching here works whether or not the index is published.

- REST: `POST /v1/workspace_query` with the engine's search:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "search", "input": {"index_id": "<index id>", "query": "renewal risk", "top_k": 10}}
```

- MCP: `workspace_query` with the same input.

### Publish an Index

On the index page, the **Publishing** card says whether the index answers outside calls. Press
**Publish**. To stop, press **Unpublish** and confirm: every outside call stops at once, and
searching in lake keeps working.

- REST: `POST /v1/workspace_act`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "expose_index", "input": {"index_id": "<index id>"}}
```

- The same with `unexpose_index` unpublishes it. MCP: `workspace_act` with the same input.

### Create a Search Key

Once the index is published, the **Search Key** card offers **Create a search key**. The key's only
scope is `search_index`, so it can search published indexes and nothing else. It is shown once:
copy it then. It keeps working for your other published indexes until you revoke it on **Tokens**,
and lasts as long as the sign-in that made it, at most 30 days.

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
  "https://lake.oramadev.net/v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index_id=<index id>&query=renewal%20risk&top_k=10"
```

- REST: `GET /v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index_id=<index id>&query=renewal%20risk`
- MCP: the tool `search_index`, with the same inputs.

| Field | Required | Meaning |
|---|---|---|
| `index_id` | yes | The index's id (`index` is an older name for it and still works). |
| `query` | yes | The text to search for. |
| `field` | no | Limit the search to one indexed field. |
| `top_k` | no | How many hits, 1 to 100 (default 10). |

The answer's `hits` are `{id, score}`, best first. Lake checks the key, checks that the index is
published, runs the search in your workspace and records the query.

## Limits

- Only a **published** model or index answers `predict` or `search_index`. Publishing is per model
  and per index.
- A model or index is reached only through lake (`/v1/predict`, `/v1/search_index` or the MCP tools
  of the same names), never at your workspace Zetta's own address.
- `search_index` answers at most 100 hits per call.
- Workspace reads time out after 15 seconds and actions after 60.
- A key lasts as long as the sign-in that made it, at most 30 days, unless your account allows keys
  that never expire ([Tokens and Keys](/docs/tokens)).

## Permissions

| Step | Needs |
|---|---|
| See models and indexes, search in the playground | `read` on the account that owns the workspace |
| Call `predict` or `search_index` | `read` on the account, and a key whose scopes include the capability |
| Publish, unpublish, create an index, create a key | `write` on the account |

## Troubleshooting

| You see | What it means | What to do |
|---|---|---|
| `unauthenticated` (401) | No key, or the key is unknown, revoked or expired; the error's `reason` says which. | Send `Authorization: Bearer <key>`; make a new key on Developer → API. |
| `unauthorized` (403) | The key is valid but does not grant this call, such as a search key calling `predict`. | Use a key whose scopes include the capability. |
| `invalid_input` at `/rows/2/tenure` | Row 2 lacks the feature `tenure`. | Send every feature the model reads; the Call drawer lists them. |
| `invalid_input` at `/workspace_id` or `/customer_id` | The workspace is unknown, or is not one of that account's workspaces. | Copy both ids from the page address or the Call drawer. |
| A refusal saying the model or index is not published | It was never published, or was unpublished. | Publish it on Ship → Models or the index page. |
| `workspace_not_ready` (503) | The workspace Zetta is starting or stopped. | Open any Design, Train or Ship page and wait until it is ready, or start it ([Workspaces](/docs/workspaces)). |
| `not_found` | No model or index with that id in this workspace. | Check the id and the workspace; ids are per workspace. |

## Related Pages

- [Train a Model](/docs/train): where models come from, and the held-out score.
- [Experiments](/docs/experiments): studies, trials and promotion.
- [Datasets and Loading Data](/docs/load-data): the datasets an index covers.
- [Apps](/docs/apps): ship a whole data app instead of an endpoint.
- [Tokens and Keys](/docs/tokens): make and revoke keys.
- [REST API and OpenAPI](/docs/developer) and [MCP Setup](/docs/mcp): calling lake from code and agents.
