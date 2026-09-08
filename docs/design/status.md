# Implementation status

What the source tree serves, by area, with the migration that holds each area's durable state and
the page that holds its contract. Production runs less than this: `Lake/prod` on AKS runs image
`oramasearch/lake:20260902c` with migrations `0001` and `0002` applied, and
[`../deployment/aks.md`](../deployment/aks.md) is the record of that state. Every area below is
implemented and tested in this repository; the ones marked *source tree* wait on a new image, the
operator's vendored schema copy, or operator volume support before they run in production.

The specification lives in [`oramasearch/liquid`](https://github.com/oramasearch/liquid) under
`docs/features/`, principally
[`18-lake-service.md`](https://github.com/oramasearch/liquid/blob/main/docs/features/18-lake-service.md).
The image `oramasearch/lake` (private) runs under the `Lake` CR on `orama-operator`; see
[`../build/container-image.md`](../build/container-image.md). GitHub Actions defines matching
pull-request builds and tagged image publishing ([`../build/ci.md`](../build/ci.md)); it becomes
active once its required credentials are configured.

## Control surface (production)

`lake` binds, serves the hello page, the `/v1` capabilities, the OpenAPI document and MCP over
Zetta's serving stack (Beast2 on Capy/Corosio), operational probes, Prometheus metrics and JSON
request logs ([`../serving/http.md`](../serving/http.md),
[`../serving/telemetry.md`](../serving/telemetry.md)). Tenants, repositories, organization
administration and policy live in the Postgres control plane
([`../api/customers.md`](../api/customers.md), [`../api/organizations.md`](../api/organizations.md),
[`../api/policy.md`](../api/policy.md)). Protected operations accept the operator bearer or a
row-resolved user credential; probes and the documented identity and transparency-monitoring
reads are unauthenticated ([`../api/auth.md`](../api/auth.md)).

## Identity (source tree)

HTTP response construction clears previous headers before CORS and routing, preventing a pooled
worker from disclosing another client's session cookie or redirect. Public entry pages remain
anonymous on reused and fresh connections; the contract and regression are in
[`../serving/http.md`](../serving/http.md#response-isolation). A containing deployment is required
to repair the live instance.

Password, TOTP with one-use recovery codes, lake-signed passkey bindings, OAuth identity,
row-resolved sessions, devices and scoped tokens (migrations `0007_identity_core`,
`0008_mfa_recovery`, `0021_token_attenuation`; [`../api/identity.md`](../api/identity.md)).
Token derivation preserves issuer scopes, expiry and ancestor revocation; static children require
existing static authority. Organization SAML/OIDC
providers, federation policy and SCIM 2.0 user/group provisioning (`0010_sso_federation`;
[`../api/sso.md`](../api/sso.md)).

Application authentication (`0016_auth_apps`) adds isolated user pools, hosted and custom-domain
identity pages, per-application JWT signing and JWKS, lifecycle webhooks, and operator pages for
application users and organizations. The registry authorizes and audits its application
management capabilities. A fresh libliquid TAAP-key assertion can issue a lake session, and that
session can complete a one-use PKCE application authorization-code flow whose ID token verifies
against the application's JWKS (`0020_identity_exit`; [`../api/apps.md`](../api/apps.md)).

Agent sessions (`0017_agent_sessions`) add short-lived Ed25519 session identities delegated to
durable agents through logged `delegation/1` attestations. The private seed is returned once and
never stored; signed drips verify as the durable agent until expiry
([`../api/agents.md`](../api/agents.md)).

Offline authorization and recovery (`0011_offline_recovery`) add bounded cached-read grants,
recovery codes, 24-hour email recovery, two-administrator organization recovery, device notices,
and designated custody of client-wrapped repository keys. Account recovery does not recreate a
lost repository key ([`../api/offline.md`](../api/offline.md),
[`../api/recovery.md`](../api/recovery.md)).

## Audit (source tree)

The signed, hash-linked audit stream records one event per capability call, sign-in and session
event; the lake TAAP identity rotates, and the stream verifies and exports (`0005_audit`;
[`../api/audit.md`](../api/audit.md)). `LAKE_INSTANCE_KEK` is required wherever a Postgres store
runs.

## Repositories and sync (source tree)

Repository lifecycle capabilities create, rename, transfer, set `private` / `organization` /
`public` visibility, and tombstone repositories (`0006_repository_lifecycle`); every outcome is
recorded through the signed audit registry ([`../api/repositories.md`](../api/repositories.md)).

Lake links libliquid as a library-only submodule and opens one customer store below
`LAKE_STORE_ROOT/<customer id>/store` ([`../deployment/store-volume.md`](../deployment/store-volume.md)).
liquid #160, #165, and #192's client, public server, protocol, and per-reference authorization
and outcome contracts are pinned. Lake serves push, pull, missing-object negotiation, chunk
upload, and ranged chunk read through the registry, with per-change and protected-reference
policy, offline-grant revocation checks, exact refusal reasons, independent reference
application, opt-in atomic refusal, and signed outcome audit ([`../api/sync.md`](../api/sync.md)).
Public repository reading through libliquid needs no session.

Signed portable repository export and import cover liquid history/object transfer, forge state,
grants, artifact evidence, an optional Git projection, and a server-rendered export download page
([`../api/export.md`](../api/export.md)). Recorded GitHub imports map Git history, issues,
reviews, and workflow documents with an explicit fidelity report
([`../api/github-import.md`](../api/github-import.md)).

## Forge (source tree)

Forge-designed repository, source, review, issue, release, webhook, and branch-protection pages
([`../ui/forge.md`](../ui/forge.md), [`../api/forge.md`](../api/forge.md)). Change pages render
libliquid's per-path structural review with safe Markdown, text hunks, line comments, modes, blob
identities, file statistics, explained and resolvable conflict values, checks, output paths, and
verified attestation profiles. A Postgres-backed scenario proves an unlanded Liquid review, cursor
movement after rewrite, approval, publication, and graph-derived evidence without repository
automation YAML.
Issue, release, webhook, and branch-protection pages expose their existing forge and signed-audit
records. Artifact pages provide filtered cursor pages of results plus result outputs, timing,
cache state, verified attestations, and retained trace summaries.

## Search (source tree)

Repository search runs over hot branch tips, immutable release and tag snapshots, exact changes,
and complete history through the registry, including REST, OpenAPI, MCP, and the `/search` page.
It persists blob-deduplicated BM25F and positional-trigram segments at
`LAKE_STORE_ROOT/<customer>/index/`, reports `hot`, `snapshot`, or `scan` mode, publishes through
atomic manifest replacement, filters candidates with the batched policy engine, and records the
signed query outcome through the registry ([`../api/search.md`](../api/search.md)).

## Platform (source tree)

Platform environments mirror append-only deployment, plain environment-variable, and domain rows
from the operator's Zetta control plane (`0019_platform`). Microsecond timestamps, per-row ordering
tokens, and a legacy fallback make mirror reduction independent of source-row order. Policy-checked capabilities and
server-rendered environment/detail/log pages remain readable from the auditable Postgres mirror
during an operator-control outage; mutations are refused until sync recovers
([`../api/platform.md`](../api/platform.md),
[`../control-plane/platform.md`](../control-plane/platform.md)).

## Metering (source tree)

Metering (`0014_metering`) aggregates hourly unique stored bytes, sync transfer, and build
compute. The `metering_usage` capability and `/metering` page apply configured integer pricing.
Per-customer quotas override the configured default and throttle new writes without changing
existing content. Organization and personal settings expose only these audited
capability-backed actions ([`../api/metering.md`](../api/metering.md),
[`../ui/settings.md`](../ui/settings.md)).

## Secrets, encryption and PKI (source tree)

Static secrets and encryption (`0009_secrets`) add customer-namespaced organization mounts,
versioned static secrets with delete/restore/destroy history, and named encryption keys with
rotation, encrypt/decrypt/rewrap, Ed25519 signatures, HMAC, derivation and customer-KEK rotation.
The registry records every operation, including mount-grant denials, and the secrets pages expose
metadata without rendering protected values or key material
([`../api/secrets.md`](../api/secrets.md), [`../api/encryption.md`](../api/encryption.md)).
`0020_identity_exit` adds encrypted, renewable and revocable dynamic-secret leases whose signed
lifecycle audit rows are committed to the transparency log.

Customer PKI (`0013_pki`) adds private root and online-intermediate authorities, SPIFFE X.509 and
JWT SVIDs, client, device, OpenSSH user and OpenSSH host certificates, and a customer-scoped
RFC 8555 ACME HTTP-01 order flow. ACME requests use one-use nonces and ES256 account JWS, and
finalization signs the submitted CSR key. Agent TAAP public keys receive short-lived certificates
with explicit rotation links, revocation, and API status enforcement
([`../api/pki.md`](../api/pki.md)). Caller exchanges persist verified non-secret metadata
([`../api/callers.md`](../api/callers.md)), while root-recovery ceremonies persist quorum,
expiry, completion, and one-way share evidence ([`../api/break-glass.md`](../api/break-glass.md)).
OIDC/JWT claim policy comes from the shared federation issuer store. All remain on the signed
audit path.

## Drive and models (source tree)

Drive presents the same repositories as folders with stable-node ACLs, server-rendered previews,
effective-access sharing before creation, expiring links with stated revocation consequences, a
shared-with-me view, bearer-link descendant traversal, preview and exact-byte downloads with
per-operation scope, expiry and revocation checks, path history, and restore-as-a-new-change
(`0015_drive` stores link expiry and revocation; [`../api/drive.md`](../api/drive.md), [`../api/sharing.md`](../api/sharing.md),
[`../ui/drive.md`](../ui/drive.md), [`../ui/sharing.md`](../ui/sharing.md)).

Models and datasets use native resumable liquid sync with chunk-level deduplication and
unique-byte metering. The Hugging Face-compatible adapter supports repository creation,
file/folder upload, basic LFS, listings, model metadata, snapshots, and named or digest
downloads. Model reads and the model page expose revision storage savings and a verified
liquid-attestation chain from the training dataset and code revision through the recorded run
([`../api/models.md`](../api/models.md)).

## Transparency log (source tree)

Threshold-signed TUF root, targets, snapshot, and timestamp metadata is served from immutable
instance-local objects under `LAKE_STORE_ROOT/<instance>/log/tuf/`. Publication refuses expired,
rolled-back, mixed, or under-threshold metadata; root rotation requires both the trusted and
replacement root thresholds, and every outcome is signed in the audit stream.

Transparency checkpoints carry signed UTC Unix-second `issued_at` values and refuse clock rollback
across publishers and restarts. Checkpoints use `LAKE_LOG_MODE`: air-gapped makes no outbound call; witnessed (the
runtime default) requires configured witness cosignatures; anchored additionally retains an
aggregation proof. `/v1/transparency_log_configuration` and `/audit` expose the effective public
configuration. The operator handoff is [`../control-plane/log.md`](../control-plane/log.md).

Canonical TAAP attestations append to immutable 1,024-entry and interior-hash tiles under the
instance log. Signed checkpoints commit to their Merkle root. Public tile and compact
inclusion/consistency-proof capabilities bind responses to exact signed checkpoints. The hosted
`/transparency` identity monitor recomputes the root before returning matches, so a modified tile
is refused. Lake pins liquid #207's client verifier, and the identity-exit scenario verifies a
dynamic-secret audit leaf against Lake's compact inclusion proof.

A configured RFC 3161 TSA timestamps checkpoint signature values into DER RFC 4998 evidence
records under `LAKE_STORE_ROOT/<instance>/log/timestamps/`. Lake verifies TSA trust, nonce,
imprint and signature, renews each record before its configured age or signer-certificate expiry
margin, and records timestamp creation, renewal, and reads through the registry audit path.

The contract for all of this is [`../api/transparency.md`](../api/transparency.md).

## Replication (source tree)

Active/standby Lake instances replicate customer repositories through durable liquid cursors and
replicate the transparency log only from a matching Merkle prefix. Signed checkpoints are verified
and adopted without rollback. The consistency model is
[`../api/replication.md`](../api/replication.md); the operator handoff is
[`../control-plane/replication.md`](../control-plane/replication.md).

## Keeping this page true

When an area lands, add or amend its paragraph here in the same change, name its migration and
its contract page, and move it out of *source tree* once the production Lake runs it. `CLAUDE.md`
carries the same status in the form the agents read first.
