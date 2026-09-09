# Implementation and deployment status

The dated [2026-09-08 production record](../deployment/production-2026-09-08.md) is the single
source for the observed `Lake/prod` deployment. It names the artifact, source revision, applied
schema, live requests, and the command for checking it again.

## Status vocabulary

| Level | Meaning |
|---|---|
| **Implemented** | The source tree contains the capability or surface described by its contract page. |
| **Exercised locally** | A local test or scenario has run the implementation, including the PostgreSQL-backed cases where marked. |
| **Deployed** | A dated live observation establishes that the deployed artifact exposes the named public surface. It does not establish full behavior. |
| **Exercised in deployment** | A dated live request or scenario completed against the deployed service. Only the actions named in the deployment record have this level. |

An OpenAPI route proves that a public route is deployed. It is not an end-to-end proof of the
capability behind that route. Contracts below remain the authoritative behavior descriptions.

## Observed deployment

`Lake/prod` runs `oramasearch/lake:20260908c` with Postgres and a bound repository-store volume;
the service reports its own source revision at `GET /v1/server_info`.
The schema ledger is applied through `0021_token_attenuation`; it contains no `0003` because no
such migration was created. The `server_info`, OpenAPI, and transparency-configuration reads are
exercised in deployment. See the [deployment record](../deployment/production-2026-09-08.md) for
the exact commands and outputs.

## Area status

| Area | Implemented | Exercised locally | Deployed | Exercised in deployment | Contract |
|---|---|---|---|---|---|
| Serving and control plane | Yes | Yes | `server_info`, OpenAPI, and Postgres server observation | `server_info` | [HTTP](../serving/http.md), [schema](../control-plane/schema.md) |
| Indexed PostgreSQL session authentication (`0022_session_lookup`) | Yes | See [measurement proof](../build/session-authentication.md) | No | No | [authentication](../api/auth.md) |
| Identity and SSO | Yes | Yes, including Postgres cases | Public routes listed in observed OpenAPI | No | [identity](../api/identity.md), [SSO](../api/sso.md) |
| Audit and authorization | Yes | Yes | Public routes listed in observed OpenAPI | No | [audit](../api/audit.md), [policy](../api/policy.md) |
| Repositories and sync | Yes | Yes, including Postgres cases | Public routes listed in observed OpenAPI | No | [repositories](../api/repositories.md), [sync](../api/sync.md) |
| Forge and export | Yes | Yes, including PostgreSQL review scenario | Public routes listed in observed OpenAPI | No | [forge](../api/forge.md), [export](../api/export.md) |
| Search | Yes | Yes | Public route listed in observed OpenAPI | No | [search](../api/search.md) |
| Platform and metering | Yes | Yes | Public routes listed in observed OpenAPI | No | [platform](../api/platform.md), [metering](../api/metering.md) |
| Secrets, encryption, PKI, and recovery | Yes | Yes, including PostgreSQL identity exit | Public routes listed in observed OpenAPI | No | [secrets](../api/secrets.md), [PKI](../api/pki.md) |
| Drive and models | Yes | Yes, including PostgreSQL drive scenario | Public routes listed in observed OpenAPI | No | [drive](../api/drive.md), [models](../api/models.md) |
| Transparency and replication | Yes | Yes | Configuration and public routes listed in observed OpenAPI | `transparency_log_configuration` | [transparency](../api/transparency.md), [replication](../api/replication.md) |
| Application and agent services | Yes | Yes | Public routes listed in observed OpenAPI | No | [applications](../api/apps.md), [agents](../api/agents.md) |
| Server-rendered UI | Yes | Yes | Not assessed by the 2026-09-08 API observations | No | [UI](../ui/README.md) |

## Keeping this page true

When behavior changes, update its contract page and this table in the same change. When a new
deployment is observed, add a dated deployment record with commands and outputs, then change only
the deployed and exercised-in-deployment cells supported by that evidence. Do not infer deployment
behavior from source tests or route discovery.
