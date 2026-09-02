# lake

**The lake binary.** The hosted service at `lake.live` — forge, drive, platform,
identity, secrets, PKI, artifact registry and search — over one content-addressed store.

liquid is the tool. lake is where it collects.

> **Status: design.** No code yet. The specification lives in
> [`oramasearch/liquid`](https://github.com/oramasearch/liquid) under `docs/features/` —
> principally [`18-lake-service.md`](https://github.com/oramasearch/liquid/blob/main/docs/features/18-lake-service.md).
> This repository becomes the implementation once the storage decision recorded there
> is made. Do not add source before then.

## What this is

One binary, C++26, sharing the chunk store and CRDT datastore with liquid. It runs on
[`oramasearch/k0s`](https://github.com/oramasearch/k0s) and is deployed by
[`oramasearch/lake-operator`](https://github.com/oramasearch/lake-operator).

| Surface | What it answers |
|---|---|
| Forge | *How did this change?* — change-based review, no CI YAML |
| Drive | *Where is my stuff?* — for people who do not think in commits |
| Platform | *Is it running?* — deploy on drip, preview environments |
| Registry | OCI, npm, PyPI, Maven, apt, models and datasets — one deduplicated store |
| Identity, secrets, PKI | TAAP/1 identities, dynamic secrets, one CA |
| Search | BM25F plus a positional trigram index, over history and inside artifacts |

## Where this sits

Part of the liquid project. See
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) for the
cross-module map and the action plan.
