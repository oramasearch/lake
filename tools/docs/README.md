# Documentation checks

`check-status.py` holds [`docs/design/status.md`](../../docs/design/status.md) to what is
deployed (lake#448). Each row of its *Area status* table names its capabilities; the script fails
when a row says **No** under *Deployed* while the observed OpenAPI lists one of them, says **Yes**
while one is missing, names a capability the source registry lacks, leaves a registry capability the
observation lacks out of every **No** row, or does not link the newest
snapshot in [`docs/deployment/observed/`](../../docs/deployment/observed/).

`DocsStatus.MatchesTheObservedOpenApi` (`tests/test_docs_status.cpp`) runs it in ctest against the
newest committed snapshot and the in-tree registry. Against the live service:

```sh
tools/docs/check-status.py --observed https://lake.example.com/v1/openapi.json
```

`--snapshot <path> --revision <rev>` records a new observation; see the status page's
*Keeping this page true*.
