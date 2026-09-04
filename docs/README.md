# lake — internal documentation

One folder per topic, one file per subject. Every change to the system lands its doc here in
the same change (bridge `CLAUDE.md`, *Documentation is part of every change*). `README.md` and
`CLAUDE.md` at the root are summaries that point here.

| Topic | Files |
|---|---|
| [`design/`](design/) | [`phase-6.md`](design/phase-6.md) — the Phase 6 design: identity on TAAP with user pools, one policy engine over one object graph, the audit stream, repositories on Zetta with liquid's push and pull, search, secrets and PKI, the waves |
| [`serving/`](serving/) | [`http.md`](serving/http.md) — the HTTP server: Beast2 on Capy/Corosio, routes, the trailing-404 rule, header stamping, log noise; [`telemetry.md`](serving/telemetry.md) — Prometheus metrics, health and readiness, request logs |
| [`control-plane/`](control-plane/) | [`postgres.md`](control-plane/postgres.md) — the Postgres the operator provisions per Lake; [`schema.md`](control-plane/schema.md) — tables, the tenant model, how the schema is applied and changed |
| [`deployment/`](deployment/) | [`aks.md`](deployment/aks.md) — `Lake/prod` on AKS: what exists, how it was built, how to roll, how to verify, what is not production-grade yet |
| [`build/`](build/) | [`ci.md`](build/ci.md) — pull-request builds, dependency cache, tagged image publishing and required credentials; [`container-image.md`](build/container-image.md) — the Linux/amd64 image and CI-built deployment tags |
| [`api/`](api/) | [`README.md`](api/README.md) — the registry, REST + OpenAPI + MCP, the envelope, adding a capability; [`auth.md`](api/auth.md) — credentials; [`identity.md`](api/identity.md) — password, TOTP, passkey, OAuth, sessions, scoped tokens and revocation; [`sso.md`](api/sso.md) — organization SAML/OIDC configuration, federation issuer policy and SCIM provisioning; [`audit.md`](api/audit.md) — signed audit events, verification, export, retention and the lake identity; [`customers.md`](api/customers.md) — users, organizations, customers; [`organizations.md`](api/organizations.md) — teams, membership, invitations, delegated administration; [`policy.md`](api/policy.md) — the object graph, roles, authorization and effective access; [`secrets.md`](api/secrets.md) — organization mounts and versioned static-secret lifecycle; [`encryption.md`](api/encryption.md) — named keys, transit operations and envelope rotation; [`mcp.md`](api/mcp.md) — JSON-RPC methods; [`repositories.md`](api/repositories.md) — repository lifecycle and customer store ownership; [`sync.md`](api/sync.md) — the published liquid repository-sync contract and lake's required server bindings |
| [`ui/`](ui/) | [`README.md`](ui/README.md) — the server-rendered pages, navigation and session cookie; [`identity.md`](ui/identity.md) — sign-up, sign-in, MFA, passkeys, sessions/devices and tokens; [`repositories.md`](ui/repositories.md) — public repository browsing, change diffs, and artifact results |

The product specification is in `oramasearch/liquid` `docs/features/18-lake-service.md`; the
cross-repo plan and decisions are in `oramasearch/liquid-bridge` `docs/`.
