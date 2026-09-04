# lake — internal documentation

One folder per topic, one file per subject. Every change to the system lands its doc here in
the same change (bridge `CLAUDE.md`, *Documentation is part of every change*). `README.md` and
`CLAUDE.md` at the root are summaries that point here.

| Topic | Files |
|---|---|
| [`serving/`](serving/) | [`http.md`](serving/http.md) — the HTTP server: Beast2 on Capy/Corosio, routes, the trailing-404 rule, header stamping, log noise |
| [`control-plane/`](control-plane/) | [`postgres.md`](control-plane/postgres.md) — the Postgres the operator provisions per Lake; [`schema.md`](control-plane/schema.md) — tables, the tenant model, how the schema is applied and changed |
| [`deployment/`](deployment/) | [`aks.md`](deployment/aks.md) — `Lake/prod` on AKS: what exists, how it was built, how to roll, how to verify, what is not production-grade yet |
| [`build/`](build/) | [`ci.md`](build/ci.md) — pull-request builds, dependency cache, tagged image publishing and required credentials; [`container-image.md`](build/container-image.md) — the Linux/amd64 image and CI-built deployment tags |
| [`api/`](api/) | [`README.md`](api/README.md) — the registry, REST + OpenAPI + MCP, the envelope, adding a capability; [`auth.md`](api/auth.md) — the admin bearer; [`customers.md`](api/customers.md) — users, organizations, customers; [`mcp.md`](api/mcp.md) — JSON-RPC methods; [`repositories.md`](api/repositories.md) — the repository model |
| [`ui/`](ui/) | [`README.md`](ui/README.md) — the server-rendered pages, navigation, the session cookie |

The product specification is in `oramasearch/liquid` `docs/features/18-lake-service.md`; the
cross-repo plan and decisions are in `oramasearch/liquid-bridge` `docs/`.
