# `Lake/prod` deployment record — 2026-09-08

This dated record captures observations of `https://lake.oramadev.net/` on 2026-09-08. It does
not claim that every advertised capability has completed an end-to-end deployment exercise.

## Running artifact and source

| Field | Value |
|---|---|
| Lake | `prod` in namespace `lake` |
| Artifact | `oramasearch/lake:20260908c` |
| Image digest | `sha256:bcbef30ac66ec89cd28dd9bc68d87553cb4e605e068633fd0efd3db053b3a06f` |
| Source revision | `9403194f4d7db225e9f909c94fcbd5d2aad64380` (`Record deployed Lake status and source provenance (#141)`) |
| Source record | **Self-reported by the running service.** `GET /v1/server_info` returns `"revision":"9403194f4d7db225e9f909c94fcbd5d2aad64380"`, and the image carries `org.opencontainers.image.revision`. The predecessor `20260908b` (`4e8216b`) predated stamping and its source was recorded by hand; from `20260908c` onward the artifact answers for itself. |
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

From `20260908c` onward the first response carries a `revision` field, so one `server_info`
request answers "which source is running" without consulting this file:

```json
{"api":"v1","name":"lake","node":"prod","revision":"9403194f4d7db225e9f909c94fcbd5d2aad64380",
 "store":"postgres","version":"0.0.1"}
```

The same revision is on the image as `org.opencontainers.image.revision`, and
`org.opencontainers.image.created` records build time. The predecessor `20260908b` predates
stamping: its response has no `revision` field, and its source (`4e8216b`) is a maintained build
record rather than something the artifact can confirm.

## Evidence commands

```text
$ kubectl -n lake get deploy lake-prod -o jsonpath='{.spec.template.spec.containers[0].image}{"\\n"}'
oramasearch/lake:20260908c

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
