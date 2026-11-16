# What lake is

lake is the hosted service around liquid: forge, drive, platform, identity, secrets, PKI,
artifact registry and search over one content-addressed store. liquid is the tool; lake is where
it collects. The product is designed for the hostname `lake.live`; the instance running today is
https://lake.oramadev.net.

## One binary

One binary, C++26. Enterprise. The control plane is Postgres, for now: the operator provisions a
CloudNativePG cluster per `Lake` and lake reaches it as `LAKE_CONTROL_DB_URI`
([`../control-plane/postgres.md`](../control-plane/postgres.md)). The data plane is Zetta, the
content-addressed store with drips and search, and lake serves the web in Zetta's style
([`../serving/http.md`](../serving/http.md)). Repository data goes through liquid's open-source
store, consumed as a library (`liquid::liquid`); liquid never depends on Zetta, and lake sits on
the other side of that licence boundary. lake runs on
[`oramasearch/k0s`](https://github.com/oramasearch/k0s) and is deployed by
[`oramasearch/orama-operator`](https://github.com/oramasearch/orama-operator) as a `Lake`
custom resource.

## Surfaces

| Surface | What it answers |
|---|---|
| Forge | *How did this change?* Change-based review, no CI YAML |
| Drive | *Where is my stuff?* For people who do not think in commits |
| Platform | *Is it running?* Deploy on drip, preview environments |
| Registry | OCI, npm, PyPI, Maven, apt, models and datasets: one deduplicated store |
| Identity, secrets, PKI | TAAP/1 identities, application authentication, agent-session keys, dynamic secrets, one CA |
| Search | BM25F plus a positional trigram index, over history and inside artifacts |

## Where this sits

lake is part of the liquid project. The product specification is `oramasearch/liquid`
`docs/features/18-lake-service.md`; the cross-module map, the action plan and the decisions are
in [`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge) `docs/`. The
Phase 6 design that shapes this codebase is [`phase-6.md`](phase-6.md); what the source tree
implements today is [`status.md`](status.md).
