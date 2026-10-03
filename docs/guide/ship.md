# Ship

## Call a model

**Ship → Models** lists every model with its project, when it was trained and its score. Press
**Publish**, then call it with a lake key. The page shows this call with your space, workspace and
model filled in; `workspace_id` is required:

```
curl -s https://lake.oramadev.net/v1/predict \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>", "rows": [{"support_tickets": 11, "tenure_months": 4}]}'
```

Each row is keyed by the model's features and carries its own key in `entity` (or the field you
name in `entity_column`); the answer ranks the rows by risk, each with its `entity` key and its
position `row`, and says what drives the scores. From an AI agent it is the MCP tool `predict`. **Unpublish** stops every outside call.

## Search an index

**Ship → Search Indexes → New index** over a dataset's text columns. Open it to search, publish
it, and create a key that can only search (`search_index`). The index page shows the call with your
values filled in:
`GET /v1/search_index?customer_id=<customer id>&workspace_id=<workspace id>&index_id=<index id>&query=renewal%20risk`.

## Services and storage

**Ship → Services** runs databases, streams and engines for your space; **Train → Data → Storage**
provisions volumes. Each shows its connection details once it is Ready.

## Build and deploy an app

Open **Ship → Apps** and use **Build an App With the ApplicationAgent**. Describe the data app you
want. ApplicationAgent may inspect datasets, schemas, read-only SQL results, repositories, Services,
and existing Apps, but its first turn cannot write anything.

The page shows the complete proposal: App settings and every repository file. Choose **Confirm and
deploy** only after reviewing them. Lake starts a second turn with that exact proposal. Only that
turn can create the private repository, commit its files, create the App, and deploy it. The result
links to the running URL. You can also choose ApplicationAgent in **Ask Orama → Talk to**, or ask Ask
Orama to delegate an app request to it; confirmation still happens before writes.
