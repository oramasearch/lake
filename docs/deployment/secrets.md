# Secrets: Two Levels

Steve, 2026-10-11 (lake#625): *"zetta should be the main secrets store... it only becomes a k8s
secret when it needs to be mounted for a deployment or app or services"*, and, correcting the first
reading: *"you're confusing the two levels of secrets... our secrets are railway env vars... but the
user needs to store their workspace secrets to launch apps... obviously our stripe secrets are not
for the user."*

Lake has two levels of secrets, and they never mix.

| Level | What | Source of truth | Where it is set or kept |
|---|---|---|---|
| **1. Orama's platform secrets** | Credentials lake and the operator use as a service: Stripe, lake's OpenAI and Clerk keys, the instance key, the control database, the admin and agent tokens, Cloudflare, Docker Hub | The hosting platform's environment | Railway variables for Orama's own deployments; on the dev cluster, Kubernetes Secrets the operator mints or a person creates, read through `spec.env` `secretKeyRef`s, never a literal in the CR. **Never in a user's secret store or a secret mount.** |
| **2. Users' workspace secrets** | What a user keeps to run their apps, services and deployments: database URLs, API keys, tokens | Lake's secret store: a secret mount sealed in the user's workspace Zetta ([../api/secrets.md](../api/secrets.md)) | A Kubernetes Secret exists only as a **projection**, written by the operator when an app, service or deployment must mount it, derived from the store and rewritten when the secret gets a new version. Never edited by hand and never the only copy. |

Level 1 is lake's configuration and stays out of this page's migration: the Stripe keys are
`LAKE_STRIPE_SECRET_KEY` and `LAKE_STRIPE_WEBHOOK_SECRET` (lake#628, [aks.md](aks.md#stripe)),
Resend's, for the order sheet's email, is `LAKE_RESEND_API_KEY` ([resend.md](resend.md)),
OpenAI's is `LAKE_OPENAI_API_KEY` ([aks.md](aks.md#speech-to-text-key)), Clerk's publishable key is
`LAKE_CLERK_PUBLISHABLE_KEY` (public by design). `LAKE_CLERK_SECRET_KEY` checks current
account restriction state at each template exchange (lake#948); a missing key or failed Backend API check
refuses sign-in. This is the instance server key, never a browser value. Clerk's webhook
signing secret is `LAKE_CLERK_WEBHOOK_SECRET` (`whsec_…`, an Orama platform key the operator injects
from the instance's Clerk configuration, orama-operator#541; without it `POST
/identity/clerk-webhook` answers 404, [../api/clerk.md](../api/clerk.md#webhooks)).

## How a Projection Works

1. The user keeps the secret in a mount of their workspace (`create_secret_mount`, `put_secret`).
   Each secret is a JSON object; each field can be one projected value.
2. The app, service or deployment names the secret, never a Kubernetes Secret: an app's secret
   environment value is `{mount_id, name, field}` (stored as
   `lake-secret://<mount id>/<name>#<field>`, [../api/web-apps.md](../api/web-apps.md#secret-environment));
   a deployment's environment names a `LakeSealedSecret` whose source is the lake secret.
3. Lake writes only that reference to the control plane (the app's desired row). No value is ever
   written to PostgreSQL (decision 51 keeps the operator on PostgreSQL; the reference is content-free
   desired state).
4. The operator turns the reference into a `LakeSealedSecret` with a `lake` source and asks lake's
   `mint_sealed_secret` for it with its own credential (the Lake's admin bearer). Lake reads the
   secret from the workspace Zetta and answers each field **sealed to the cluster's delivery key**
   (X25519, XChaCha20-Poly1305, bound to the target namespace and Secret name;
   [../api/secrets.md](../api/secrets.md#projections)). Only the operator, holding that key's private
   half, can open it, and it writes the Kubernetes Secret.
5. **Rotation.** The operator delivers an unpinned source again every five minutes, so a new version
   put in lake reaches the Secret; for an app, the new value changes the copied Secret's digest and
   the app's pods roll.
6. Removing the reference removes the `LakeSealedSecret`, its Secret and the app's copy.

The decision and its alternatives are decision 58 in liquid-bridge `docs/decisions.md`.

## Audit: How User Apps, Services and Deployments Get Their Secrets (2026-10-11)

Read on the dev cluster (`orama-production-cluster-01`, namespaces `lake`, the workspace namespaces
`prod-ws-*`/`dev-ws-*` and the build namespace; `orama-prod2` was not read). Names and keys only;
no value was printed. `<id>` is the first 12 hex of an app, service, launch or workspace id.

| Flow | Kubernetes Secret today | Keys | Who reads it, how | Class | After lake#625 |
|---|---|---|---|---|---|
| **Ship app secret environment** | Before: a Secret in `lake` named by the app's `secret_env_refs`, copied to `app-<id>-env-<hash>` in the workspace namespace. On dev **no app uses one** (0 of 3 apps) | the referenced keys | The app container, `env[].valueFrom.secretKeyRef` on the copy | **Moves into lake's store** | Lake refuses a Kubernetes Secret name; the reference names a lake secret. The operator owns `LakeSealedSecret` `app-<id>-lake-<hash>` in `lake` (source `lake`), lake delivers it sealed, the field is copied into the workspace namespace and refreshed every five minutes |
| **Deployments' secrets** (`LakeEnvironment` `spec.secretRefs`) | `LakeSealedSecret` resources in `lake` and their produced Secrets, copied to the environment namespace. On dev **none exist** | the secret's fields | The workload, `envFrom` on the copy | **Projection from lake's store** | A `LakeSealedSecret` with a `lake` source now completes against lake (`mint_sealed_secret` was missing until now). Plain environment values (`set_environment_env`) refuse anything that looks like a secret |
| **Service connections** (managed Postgres, Valkey, Kafka and other services) | `prod-svc-<id>-app` (CloudNativePG basic-auth), `-ca`, `-server`, `-replication` in the service's namespace; each Ready service's connection document in `lake-prod-services` | `user`, `password`, `uri`, `host`, `port`, `dbname`, `jdbc-uri`, `pgpass`…; `ca.crt`/`ca.key`; `tls.crt`/`tls.key`; `<service id>`, `svc-<id>.json` | The database itself (the service's operator mounts them); lake through the volume `services` (`lake-prod-services`); a bound app through `app-<id>-bindings` | **Mount projection**, generated by the service's operator | Unchanged. The credential is minted by the service, not kept by the user; mirroring it into the workspace's secret store so a user can read and rotate it in lake is lake#629 |
| **App bindings** | `app-<id>-bindings` in the workspace namespace | one key per bound service id | The app, volume at `/var/run/orama/bindings` | **Mount projection** of the service connections above | Unchanged |
| **App tokens** | `app-<id>-tokens` in the workspace namespace | `events`, `pull`, `source` | The app, `env` `EVENTS_TOKEN`, `APP_TOKEN` | **Mount projection**, operator-minted per app (only SHA-256 hashes reach PostgreSQL) | Unchanged |
| **Registry pull for user images** | `app-<id>-pull` in the workspace namespace; `builder-pull` in `lake-prod-builds-<id>` | `.dockerconfigjson` | The kubelet, `imagePullSecrets`; the build Job | **Mount projection**, operator-minted lake registry tokens | Unchanged |
| **Agent launches** (a user's agents and schedules) | `prod-al-<id>-credential`, `prod-as-<id>-credential` in `lake` or the workspace namespace | `token` | The agent container, `env` `LAKE_TOKEN` | **Mount projection**, operator-minted task credentials | Unchanged |

Level 1 Secrets seen in the same namespaces, listed so nothing is unaccounted for and left as they
are: `lake-prod-instance-kek` (`key`, the instance key: bootstrap), `lake-prod-controldb-*` (the
control database: bootstrap), `lake-prod-admin`, `lake-prod-agent-service-token`,
`lake-prod-agent-bus` (operator-minted lake and agent credentials), `lake-prod-sealing-key` (the
cluster delivery key; never mounted), `lake-prod-zetta-zetta-*` and `prod-ws-<id>-zetta-*` (Zetta
admin and cluster tokens), `lake-prod-openai` (`OPENAI_API_KEY`, lake's OpenAI key; agents reach models through lake's model gateway and hold none once `spec.agents.openaiSecretName` is removed, lake#248),
`cloudflare-api-token` (`accountID`, `token`, `zoneID`; the operator's DNS records, from
`spec.dnsSecretRef`), `oramasearch-dockerhub` (Orama's private images), and in `orama-system` the
operator's own (`cloudflare-api-token`, `cloudflare-r2`, `oramasearch-dockerhub`), the Cloud v1 dev
stack's `v1-*` Secrets and Helm release records.

## Roll Out (Steve's Steps)

Nothing on dev needs a value moved: no app has a secret reference and no `LakeSealedSecret` exists.
The level 1 Secrets stay where they are. In order:

1. **Lake** with `mint_sealed_secret` (this change) rolls first; nothing asks it for a delivery yet.
2. **The operator's CRDs.** Helm does not upgrade `crds/`, so apply the two changed CRDs from
   orama-operator (branch `lake-625-secret-projections`) before the operator:
   `kubectl --context orama-production-cluster-01 apply -f chart/crds/orama.cloud_lakeapps.yaml -f chart/crds/orama.cloud_lakesealedsecrets.yaml`.
3. **The operator** (`helm upgrade`, per orama-operator `docs/development.md`). A `helm upgrade` also
   reconciles Prod 2: afterwards confirm the `OramaCloudV1` in `orama-prod2` is still `Ready`.
4. **Use it.** A person keeps the secret in a mount of the app's workspace and names it on the app,
   never handling the value outside lake: the Secrets pages (`/secret-mounts`,
   [../ui/secrets.md](../ui/secrets.md)), or `create_secret_mount` then `put_secret`, then the app's Settings → Environment with the mount, secret and field. The
   `LakeSealedSecret` appears in `lake` within a reconcile, and the app rolls once it is delivered.
5. **Removing an old Kubernetes Secret** is a person's call, made only after the app reads from the
   store. None exists on dev today.

## Verify

```sh
# A user's app reads a lake secret: its reference, never a value.
curl -sS -H "Authorization: Bearer $TOKEN" "$LAKE/v1/get_app?customer_id=$C&workspace_id=$W&app_id=$APP" \
  | jq '.value.secret_env_refs'
# The projection the operator owns for it, and the version it holds.
kubectl --context orama-production-cluster-01 -n lake get lakesealedsecret \
  -l lake.orama.cloud/app-lake-secret=true \
  -o custom-columns=NAME:.metadata.name,PHASE:.status.phase,VERSION:.status.version,MINTED:.status.mintedAt
```

Tests: `SealedDelivery.*` and `MintSealedSecret.*` (`tests/test_sealed_delivery.cpp`): the envelope
byte for byte against the operator's reference fixture, delivery of every field sealed and never a
value, rotation, a pinned version, operator-only, the cluster's key only, a dynamic source, and an
app's references. The operator's `TestLakeSourceFollowsTheSecretsCurrentVersion`,
`TestLakeSourceNeedsTheLakesAdminSecret`, `TestLakeAppRowSecretEnvironmentContract` and
`TestLakeAppsEnvtestLifecycle` cover its side.

## Change It

- A new flow that needs a user's secret mounted names a lake secret and gets a `LakeSealedSecret`
  with a `lake` source; never a Kubernetes Secret a person creates.
- A new platform credential is a level 1 environment variable of the hosting platform, documented in
  [aks.md](aks.md), never a secret mount.
