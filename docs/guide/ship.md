# Ship

## Call a model

**Ship → Models** lists every model with its project, when it was trained and its score. Press
**Publish**, then call it with a lake key:

```
curl -s https://lake.oramadev.net/v1/predict \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "…", "model": "<model id>", "rows": [{"support_tickets": 11, "tenure_months": 4}]}'
```

Each row is keyed by the model's features; the answer ranks the rows by risk with what drives each
score. From an AI agent it is the MCP tool `predict`. **Unpublish** stops every outside call.

## Search an index

**Ship → Search Indexes → New index** over a dataset's text columns. Open it to search, publish
it, and create a key that can only search (`search_index`).

## Services and storage

**Ship → Services** runs databases, streams and engines for your space; **Train → Data → Storage**
provisions volumes. Each shows its connection details once it is Ready.
