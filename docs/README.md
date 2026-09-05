# lake — internal documentation

One folder per topic, one file per subject. Every change to the system lands its doc here in
the same change (bridge `CLAUDE.md`, *Documentation is part of every change*). `README.md` and
`CLAUDE.md` at the root are summaries that point here.

| Topic | Files |
|---|---|
| [`design/`](design/) | [`phase-6.md`](design/phase-6.md) — the Phase 6 design: identity on TAAP with user pools, one policy engine over one object graph, the audit stream, repositories on Zetta with liquid's push and pull, search, secrets and PKI, the waves; [`ui/mocks/README.md`](design/ui/mocks/README.md) — quiet, Orama-family and editorial visual directions for repository, change review and organization pages, with rendered screenshots |
| [`serving/`](serving/) | [`http.md`](serving/http.md) — the HTTP server: Beast2 on Capy/Corosio, routes, the trailing-404 rule, header stamping, log noise; [`telemetry.md`](serving/telemetry.md) — Prometheus metrics, health and readiness, request logs |
| [`control-plane/`](control-plane/) | [`postgres.md`](control-plane/postgres.md) — the Postgres the operator provisions per Lake; [`schema.md`](control-plane/schema.md) — tables, the tenant model, how the schema is applied and changed; [`log.md`](control-plane/log.md) — transparency-log runtime configuration the operator maps from the Lake CR; [`replication.md`](control-plane/replication.md) — active/standby topology, fencing, reconciliation, and promotion contract |
| [`deployment/`](deployment/) | [`aks.md`](deployment/aks.md) — `Lake/prod` on AKS: what exists, how it was built, how to roll, how to verify, what is not production-grade yet |
| [`build/`](build/) | [`ci.md`](build/ci.md) — pull-request builds, dependency cache, tagged image publishing and required credentials; [`container-image.md`](build/container-image.md) — the Linux/amd64 image and CI-built deployment tags; [`testing.md`](build/testing.md) — process-unique filesystem fixtures and concurrent Postgres suite isolation |
| [`api/`](api/) | [`README.md`](api/README.md) — registry and surfaces; [`apps.md`](api/apps.md) — application user pools, hosted identity, JWTs and webhooks; [`drive.md`](api/drive.md) — folders, previews and path history; [`sharing.md`](api/sharing.md) — live grants, effective access and expiring links; [`models.md`](api/models.md) — model cards, provenance and compatible downloads; [`auth.md`](api/auth.md) — credentials; [`identity.md`](api/identity.md) — identity; [`offline.md`](api/offline.md) and [`recovery.md`](api/recovery.md) — offline and recovery; [`sso.md`](api/sso.md) — SSO/SCIM; [`audit.md`](api/audit.md) — audit; [`transparency.md`](api/transparency.md) — TUF roles, threshold signing, tiled monitoring and key-compromise recovery; [`replication.md`](api/replication.md) — asynchronous repository and log replication with the consistency model; [`forge.md`](api/forge.md), [`export.md`](api/export.md), [`github-import.md`](api/github-import.md) — collaboration and migration; [`customers.md`](api/customers.md), [`organizations.md`](api/organizations.md), [`policy.md`](api/policy.md) — control plane; [`search.md`](api/search.md) — search; [`secrets.md`](api/secrets.md), [`encryption.md`](api/encryption.md), [`pki.md`](api/pki.md), [`callers.md`](api/callers.md), [`break-glass.md`](api/break-glass.md) — protected services; [`mcp.md`](api/mcp.md) — MCP; [`repositories.md`](api/repositories.md) and [`sync.md`](api/sync.md) — repository data and sync |
| [`ui/`](ui/) | [`README.md`](ui/README.md) — the server-rendered pages, navigation and session cookie; [`design-system.md`](ui/design-system.md) — the pinned Orama token pipeline and UI component rules; [`identity.md`](ui/identity.md) — identity pages; [`repositories.md`](ui/repositories.md) — repository pages; [`drive.md`](ui/drive.md) — folder, preview, sharing, history, and Linux client states |

The product specification is in `oramasearch/liquid` `docs/features/18-lake-service.md`; the
cross-repo plan and decisions are in `oramasearch/liquid-bridge` `docs/`.

Customer usage, integer pricing, and quota behavior are documented in
[`api/metering.md`](api/metering.md).
