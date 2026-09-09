# `Lake/prod` deployment record — 2026-09-08

This dated record captures observations of `https://lake.oramadev.net/` on 2026-09-08. It does
not claim that every advertised capability has completed an end-to-end deployment exercise.

## Running artifact and source

| Field | Value |
|---|---|
| Lake | `prod` in namespace `lake` |
| Artifact | `oramasearch/lake:20260908b` |
| Image digest | `sha256:dfcff7dc6fffcfa41fa54876f3d20af6339d7413d9766537d81180b3623bd8b0` |
| Source revision | `4e8216b60f3ac2a154c8e1438a5298f4f4c224d6` (`Present Lake's customer repository journey (#139)`) |
| Source record | Maintained by the person who ran this build. This image predates image and API revision stamping, so the artifact cannot confirm the source revision. |
| Schema asset | `sha256:9aec095673cf`, status `Applied`, guard `token_attenuation_guard` |

The deployment has one ready Lake replica and a bound `lake-prod-store` volume mounted at
`/var/lib/lake/store` as `LAKE_STORE_ROOT`.

## Applied schema

The `schema_migrations` ledger contains `0001_identity_and_customers`, `0002_repositories`, and
`0004_organizations_policy` through `0021_token_attenuation`. `0003` is absent by design: the
schema history began with migrations `0001` and `0002`, then assigned `0004` to the organization
and policy addition; no `0003` migration was created. The ledger is the authoritative applied
revision list.

## Observed public surfaces

The following requests returned HTTP 200 on 2026-09-08:

```text
GET /v1/server_info
{"duration_ms":0.007612,"ok":true,"value":{"api":"v1","name":"lake","node":"prod","store":"postgres","version":"0.0.1"}}

GET /v1/openapi.json
HTTP 200

GET /v1/transparency_log_configuration
{"duration_ms":0.007782,"ok":true,"value":{"mode":"air-gapped","witness_threshold":0,"witnesses":[]}}
```

The OpenAPI document lists identity, repository-sync, forge review, search, drive, platform,
metering, protected-service, transparency, replication, application, and agent capability routes.
This establishes that those public surfaces are deployed. It does not establish that every route
or capability behaves end to end.

`server_info`, `openapi`, and `transparency_log_configuration` were exercised in deployment by
the requests above. No other capability is marked exercised in deployment by this record.

## Smoke check

Run this read-only command to compare the active artifact and public deployment record:

```sh
base=https://lake.oramadev.net
kubectl -n lake get deploy lake-prod \
  -o jsonpath='{.spec.template.spec.containers[0].image}{"\\n"}'
curl -sS --fail-with-body "$base/v1/server_info" \
  | jq '{ok, value: {name: .value.name, node: .value.node, store: .value.store, version: .value.version, revision: .value.revision}}'
curl -sS --fail-with-body "$base/v1/openapi.json" \
  | jq -e '.paths["/v1/identity"] and .paths["/v1/search"] and .paths["/v1/drive_browse"] and .paths["/v1/create_review"]'
curl -sS --fail-with-body "$base/v1/transparency_log_configuration" \
  | jq '{ok, value: {mode: .value.mode, witness_threshold: .value.witness_threshold, witnesses: .value.witnesses}}'
```

On `20260908b`, the first response has no `revision` field because it predates revision stamping;
compare its image with this record and treat the listed source revision as the maintained build
record. Images built by the updated workflow include the full revision in both the OCI label
`org.opencontainers.image.revision` and `server_info`, so one `server_info` response can be
compared directly with a deployment record. `org.opencontainers.image.created` records build time.

## Evidence commands

```text
$ kubectl -n lake get deploy lake-prod -o jsonpath='{.spec.template.spec.containers[0].image}{"\\n"}'
oramasearch/lake:20260908b

$ kubectl -n lake exec lake-prod-controldb-1 -c postgres -- psql -U postgres -d lake -Atc 'SELECT version FROM schema_migrations ORDER BY version'
0001_identity_and_customers
0002_repositories
0004_organizations_policy
0005_audit
0006_repository_lifecycle
0007_identity_core
0008_mfa_recovery
0009_secrets
0010_sso_federation
0011_offline_recovery
0012_forge_export_import
0013_pki
0014_metering
0015_drive
0016_auth_apps
0017_agent_sessions
0018_settings_surface
0019_platform
0020_identity_exit
0021_token_attenuation
```
