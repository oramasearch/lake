# Control-plane schema

**Source of truth: `db/schema.sql` in this repository.** The operator carries a byte-identical
vendored copy at `internal/controller/assets/lake-schema.sql` and applies it to every Lake's
Postgres. Change the schema here, then copy it into the operator in the same change; the
operator's test checks the copy carries the source-of-truth header.

## Tables (migration `0001_identity_and_customers`)

`schema_migrations(version, applied_at)` records what has been applied.

**Identity** (who; owns nothing):

| Table | Columns |
|---|---|
| `users` | `id uuid` (v4, `gen_random_uuid()`), `taap_id text UNIQUE` (the person's TAAP identity `taap:1:<alg>:<base32>`, null until they have a key), `email text` (unique on `lower(email)`), `display_name`, `created_at`, `updated_at` |
| `organizations` | `id uuid`, `taap_id text UNIQUE`, `display_name`, `created_at`, `updated_at` |

**Tenant** (the customer owns everything; every owned table will carry `customer_id … ON DELETE
CASCADE`):

| `customers` | |
|---|---|
| `id uuid` | v4 |
| `customer_type text` | `individual` or `organization` |
| `user_id uuid UNIQUE` → `users`, `organization_id uuid UNIQUE` → `organizations` | nullable; the UNIQUEs give exactly one customer per user and per organization (NULLs don't collide); `ON DELETE CASCADE` |
| `customer_type_check` | a customer is one or the other, never both: the type ties to exactly one non-null identity column |
| `name varchar(256) NOT NULL` | the handle: **one global namespace** shared by individuals and organizations (`customer_name_unique`); lowercase letters, digits, inner hyphens, 1–256 chars (`customer_name_format`), so uniqueness is effectively case-insensitive and a handle can never carry spaces or uppercase. No default: the application generates handles (`{adjective}-{noun}-{number}`) and must pre-check uniqueness to answer 409 rather than 500 |

**Repositories** (migration `0002_repositories`):

| `repositories` | |
|---|---|
| `id uuid` | v4 |
| `customer_id uuid NOT NULL` → `customers` | the owner; `ON DELETE CASCADE`; indexed |
| `name varchar(256) NOT NULL` | slug in the handle format (`repository_name_format`); `UNIQUE (customer_id, name)` so the address is `<customer>/<name>` |
| `description text` | default empty |
| `visibility text` | `private` (default) or `public` (`repository_visibility_check`) |

`updated_at` is maintained by a trigger (`set_updated_at()`) on every table.

## Organizations and authorization (migration `0004_organizations_policy`)

| Table | Purpose |
|---|---|
| `objects` | the authorization graph: type, stable external id, optional parent and `inherit` cut flag |
| `teams` | named teams within an organization |
| `memberships` | user or workload membership in an organization or team, its `owner` / `admin` / `member` role, TAAP attestation digest and granting identity |
| `grants` | principal, policy object and role, with the TAAP attestation digest and granting identity |
| `invitations` | email or handle invitations, expiry, state and accepted membership |

The migration backfills policy objects for existing customers, organizations and repositories.
New rows add their policy objects in the same store transaction. Parent topology and the role
matrix are validated by the stores; graph traversal and action evaluation are documented in
[`../api/policy.md`](../api/policy.md).

This mirrors the Orama Cloud v1 tenant model (user and organization are identity, customer is
the tenant, one handle namespace) and fixes its two known gotchas at the constraint level:
case-sensitivity and unvalidated handle format.

## Audit (migration `0005_audit`)

| Table | Columns and invariants |
|---|---|
| `lake_identity` | Every historical lake TAAP/1 identity: UUID, TAAP ID, public key, wrapped generated Ed25519 seed (AES-256-GCM under `LAKE_INSTANCE_KEK`), creation timestamp, optional dual-signed rotation document, and a partial-unique `current` row. An operator-supplied seed (`LAKE_IDENTITY_SEED`) is used in place of a generated one. |
| `audit_events` | Ordered `sequence`, UUID, timestamp, optional customer, kind, principal, object, action, outcome (`success`, `failure`, `denied`, `revoked`), the request id as text (clients may send `x-request-id`), optional method and device, JSON details, predecessor digest, signer, signature and digest. UPDATE and DELETE are rejected by triggers. |
| `audit_retention` | One retention setting per customer, constrained to at least 400 days, timestamped on update; the application refuses to lower it. |

Every capability call, sign-in and session event is one row, written by `audit::record` under a
transaction advisory lock that reads the latest digest and appends the next event; the signature
and digest chain make a missing or reordered row detectable to `verify_audit`. The provisional
`audit_events` shape that migration `0004` carried is gone: the store methods no longer write
their own rows, because `Registry::call` records every outcome.

## Repository lifecycle (migration `0006_repository_lifecycle`)

`repositories` gains nullable `deleted_at` and `retention_until` timestamps. Its visibility
constraint also accepts `organization`, between private and public. `repository_refs` holds the
current repository heads. Lifecycle operations update
the repository and its policy-object parent in one store transaction; the registry records each
attempt in the signed audit stream.

## Identity core (migration `0007_identity_core`)

| Table | Purpose |
|---|---|
| `identity_documents` | Verified TAAP/1 document and SHA-512/256 digest for each user. |
| `credentials` | Pool-scoped password, TOTP, passkey, OAuth, or admin proof; secrets are hashed or encrypted. Passkey public data carries its device link and lake-signed TAAP binding. |
| `sessions` | Browser, API-token, static, or admin bearer hash; scopes, expiry, revocation, and last use. |
| `devices` | Named user device and revocation state; `list_devices` joins passkey binding data from its credential. |
| `keystore` | Purpose-specific data key wrapped by the instance KEK. |
| `oauth_providers` | Pool-scoped provider configuration and encrypted client secret. |
| `identity_oauth_flows` | Five-minute, one-use OAuth state, encrypted PKCE verifier, nonce, and initiating device. |
| `identity_webauthn_challenges` | Five-minute, one-use registration/assertion challenge with pool, RP ID, and origin. |
| `identity_core_guard` | Guard table for the identity-core migration. |

Migration numbers `0004`–`0006` landed first from parallel branches, so identity core follows as
`0007`. It uses the signed `audit_events` and `lake_identity` tables from migration `0005`; it does
not recreate the provisional audit shape from the earlier identity checkpoint.

## MFA recovery (migration `0008_mfa_recovery`)

The `credentials` kind constraint accepts `recovery`. Each MFA enrollment stores its TOTP seed as
an encrypted `totp` credential and eight independently Argon2id-hashed recovery credentials.
Successful recovery-code verification revokes that row immediately. Re-enrollment and verified
disablement revoke all prior recovery rows. `identity_mfa_guard` is the migration guard.

## Static secrets and encryption (migration `0009_secrets`)

| Table | Purpose |
|---|---|
| `secrets_mounts` | Organization mount inside a customer namespace; unique lowercase name per customer and organization. |
| `secrets_entries` | One static-secret name, current-version pointer and JSON custom metadata per mount. |
| `secrets_versions` | Immutable numbered ciphertext plus soft-delete and irreversible-destroy timestamps. Destroy clears ciphertext but retains the history row. |
| `secrets_encryption_keys` | Named key metadata, current version and optional derived-parent link. |
| `secrets_data_keys` | Each mount data key and each named-key version, wrapped by its customer KEK. |
| `keystore` | Gains nullable `customer_id`; one unique row holds each customer KEK wrapped by `LAKE_INSTANCE_KEK`. Existing purpose rows remain valid. |
| `secrets_guard` | Guard table for migration `0009_secrets`. |

The secret tables carry the area prefix required for parallel schema work. Mount creation verifies
that the organization policy object is below the named customer and creates the corresponding
environment and mount policy objects in the same transaction. The first value for a name creates
the child secret object.

Static values use one mount data key and associated data containing mount, name and version.
Named encryption keys have one data-key row per version. Customer-KEK rotation locks the customer
KEK and all of its data-key rows, rewraps those rows and updates the KEK in one transaction. It
does not update `secrets_versions`.

## SSO and federation (migration `0010_sso_federation`)

| Table | Purpose |
|---|---|
| `sso_providers` | Organization and pool-scoped SAML or OIDC configuration: issuer, metadata, signing certificate, OIDC client settings, mapped attributes, JIT setting, SCIM token hash, test state, and enabled state. OIDC client secrets are encrypted. |
| `federation_issuers` | The acceptable `(organization, issuer, claim)` triples. An SSO assertion or token exchange may use a claim only when this table admits its issuer. |
| `scim_users` | Stable SCIM external-user to lake-user mapping, active state, and source attributes. Deactivation revokes the mapped user's sessions in the same transaction. |
| `scim_groups` | Stable SCIM external-group to lake-team mapping. |
| `scim_group_members` | Source-managed group membership and its resulting organization-team membership. |
| `sso_provisioning_logs` | Immutable operational evidence for SCIM create, update, and deactivation outcomes. |
| `sso_federation_guard` | The migration guard. |

The credential-kind constraint also accepts `saml` and `oidc`; both remain proofs bound to the
user's TAAP identity, rather than identities in their own right.

## Offline authorization and recovery (migration `0011_offline_recovery`)

| Table | Purpose |
|---|---|
| `offline_recovery_grants` | Lake-signed, user/device-bound cached-read grants with explicit scopes, expiry, and revocation. |
| `offline_recovery_attempts` | Email or administrator recovery state, cooling/expiry timestamps, hashed one-use token, and completion. |
| `offline_recovery_admin_approvals` | Distinct approving TAAP identities; the composite key prevents an administrator from approving twice. |
| `offline_recovery_deliveries` | Operator mail outbox. Claiming sets `delivered_at` and clears the plaintext token. |
| `offline_recovery_notifications` | Immutable recovery notices addressed to every device registered to the affected user. |
| `offline_recovery_contacts` | One designated TAAP contact and opaque client-wrapped trust-domain key per repository. |
| `offline_recovery_guard` | Guard table for migration `0011_offline_recovery`. |

All new tables carry the area prefix. Account attempts and grants cascade with the user; key
custody cascades with the repository. Lake stores only the recovery token hash on an attempt and
never stores an unwrapped repository key. The API and security consequences are in
[`../api/offline.md`](../api/offline.md) and [`../api/recovery.md`](../api/recovery.md).

## Forge, export, and GitHub import (migration `0012_forge_export_import`)

| Table | Purpose |
|---|---|
| `forge_records` | Versioned, repository-owned issue, review, release, webhook, branch-protection, export, and GitHub-import facts. |
| `forge_guard` | Guard table for the migration. |

The `value` column is a JSON object with a versioned application shape. This deliberately keeps a
review anchor as a durable value rather than a fixed collection of columns: the temporary
line-hunk mapper can be replaced by liquid #174's `map_anchor` result without rewriting stored
comments. `forge_records` is the area prefix for parallel Phase 6 schema work.

## PKI, callers, and break-glass (migration `0013_pki`)

| Table | Purpose |
|---|---|
| `pki_authorities` | One private CA hierarchy per customer. Root and online-intermediate private keys are ciphertext wrapped by the customer KEK; only the public chain is readable from the row. Customer-KEK rotation rewraps both ciphertext columns. |
| `pki_certificates` | Issued short-lived X.509, JWT, ACME, SSH, client, and device credential metadata and public output. |
| `pki_acme_orders` | ACME order state, identifiers, account key reference, expiry, and issued certificate link. |
| `pki_callers` | Short-lived workload credentials minted after Kubernetes, federation, AWS, or Azure attestation. |
| `pki_recovery_ceremonies`, `pki_recovery_shares` | Configured Shamir quorum, recovery evidence digests, expiry, and completed-rehearsal state. Shares are never retained. |
| `pki_guard` | Guard table for this migration. |

Caller rows retain verified identity metadata and expiry, never the returned bearer credential.
Recovery rows retain ceremony configuration, completion state, and SHA-256 evidence digests,
never share values or reconstructed root material. Both writes complete before their registry
capability succeeds and therefore correlate with the capability's signed audit event.

The migration follows forge/export/import migration `0012`.

## Metering (migration `0014_metering`)

| Table | Purpose |
|---|---|
| `metering_usage` | One row per customer and UTC hour. Unique stored bytes are a gauge; inbound transfer, outbound transfer, and compute seconds are counters. |
| `metering_build_records` | Customer/result idempotency ledger that adds a build's rounded compute seconds to its hour once. |
| `metering_guard` | Guard table for this migration. |

Both data tables use the metering area prefix. Customer deletion cascades its usage and ledger.
The migration follows PKI migration `0013`.

## Settings quota (migration `0018_settings_surface`)

| Table | Purpose |
|---|---|
| `customer_storage_quotas` | Explicit per-customer storage quota override used by metering and repository-write enforcement. A zero value disables that customer's quota. |
| `settings_guard` | Guard table for the settings-surface migration. |

Customers without a row continue to use `LAKE_STORAGE_QUOTA_BYTES`. The operator schema copy is
deferred to the wave-level operator migration pull request.

## Drive sharing (migration `0015_drive`)

| Table | Purpose |
|---|---|
| `drive_share_links` | Folder object, stable liquid node id, path, role, HMAC token digest, expiry, creator, and optional revocation for a bearer link. |
| `drive_guard` | Guard table for this migration. |

Direct recipient shares remain ordinary `grants`; the link row adds expiry and revocation to a
synthetic `share-link:<id>` principal grant. Repository deletion cascades links. Tokens are never
stored. The migration follows metering migration `0014`.

## Application authentication (migration `0016_auth_apps`)

| Table | Purpose |
|---|---|
| `auth_applications` | Registration, isolated pool, exact origins and redirects, custom host, and theme. |
| `auth_app_signing_keys` | Per-application Ed25519 public keys and encrypted signing seeds, including rotation state. |
| `auth_app_users` | Pool membership and active or suspended status for application users. |
| `auth_app_organizations` | Application-facing organization profiles and metadata. |
| `auth_app_webhooks` | Event filters, destination, enabled state, and encrypted signing secret. |
| `auth_app_webhook_events` | Immutable lifecycle event payloads. |
| `auth_app_webhook_deliveries` | Every delivery attempt, outcome, signature, and error. |
| `auth_app_guard` | Guard table for this migration. |

`users.pool_id` scopes password, OAuth, passkey, session, and MFA lookup. The migration replaces
the former global credential indexes with pool-qualified indexes and creates an `application`
policy object for each registration. Application key seeds and webhook secrets are encrypted by
the instance keystore before persistence. The migration follows drive migration `0015`.

## Agent sessions (migration `0017_agent_sessions`)

| Table | Purpose |
|---|---|
| `agent_sessions` | Customer-owned session UUID, durable and dispatcher TAAP identities, public session key, scopes, expiry, canonical delegation attestation, and its transparency-log index and root. |
| `agent_guard` | Guard table for this migration. |

The session private seed is returned once by the mint capability and never stored. Expired rows
remain as public attribution evidence, while live verification rejects them at or after
`expires_at`. Customer deletion cascades the records. The migration follows application
authentication migration `0016`.

## Platform mirror (migration `0019_platform`)

| Table | Purpose |
|---|---|
| `platform_environments` | Repository/name binding to the local environment policy object created when the mirror first sees an environment. |
| `platform_mirror` | Last operator-control JSON row Lake saw per `(kind, key)`, plus local `seen_at`; deployment, environment-value, and domain rows share this auditable cache. |
| `platform_guard` | Guard table for the platform migration. |

The issue brief reserved `0017_platform`, but `0017_agent_sessions` and `0018_settings_surface`
were already present on main. The platform tables therefore use the next collision-free migration,
`0019_platform`. Operator Zetta remains the source of truth; these tables provide stale reads and
local policy binding only.

## Identity exit scenario (migration `0020_identity_exit`)

| Table or change | Purpose |
|---|---|
| `identity_taap_assertions` | One-use digest ledger binding each accepted liquid-key sign-in assertion to its lake user and pool. |
| `auth_app_authorization_codes` | Five-minute application code hashes with exact redirect, PKCE S256 challenge, nonce, user, and one-use timestamp. |
| `pki_certificates.revoked_at` and `agent` profile | Immediate agent-certificate status plus rotation linkage through the existing `renewed_from_id`. |
| `secrets_dynamic_leases` | Encrypted generated credential, role, bounded expiry, revocation, and renewal count for each mount lease. |
| `identity_exit_guard` | Guard table for migration `0020_identity_exit`. |

Dynamic-secret ciphertext uses the existing mount/customer key hierarchy. Authorization codes and
TAAP assertions retain hashes rather than bearer values. Certificate rows retain only public
certificate output and lifecycle metadata.

## How it is applied

With `spec.postgres` on, the operator owns, per Lake, a ConfigMap `lake-<name>-controldb-migrations`
(the SQL) and a `PostgresSchema` `lake-<name>-controldb-schema` whose controller runs an idempotent
Job: wait for the DB, skip if the guard table exists, apply the whole file in **one transaction**
with `psql --single-transaction`. **The guard is the table the newest migration creates**
(`token_attenuation_guard` in the source tree), so a new migration means a new guard and the next Job re-runs the
idempotent file, adding only what is missing. The version is a content hash of the SQL
(`sha256:<12 hex>`); `kubectl get lake` shows it as `SCHEMA`.

**Migrations never run in lake pods**, so the replica count cannot duplicate or race them.

## How to change it

Today the mechanism is single-shot: once the guard table exists, a changed version is a no-op
(operator issue #126 tracks incremental migrations). Until that lands:

1. Edit `db/schema.sql` idempotently (`IF NOT EXISTS`, `CREATE OR REPLACE`, `DROP … IF EXISTS`
   before `CREATE TRIGGER`), append a new `INSERT INTO schema_migrations`.
2. Copy to the operator's `assets/lake-schema.sql`; the version hash changes automatically. During
   the Phase 6 waves this copy and the `GuardTable` update land once per wave in a separate operator
   pull request, never a partial schema between waves. By the approved Phase 6 wave-level waiver,
   the operator copies for `0009_secrets` and `0010_sso_federation` are deferred to the separate
   operator migration pull request that collects this wave; lake keeps the source-of-truth schema
   and newest guard ready for that update. The forge wave adds `0012_forge_export_import` and
   `forge_guard`; the metering wave adds `0014_metering` and `metering_guard`; drive adds
   `0015_drive` and `drive_guard`; application authentication adds `0016_auth_apps` and
   `auth_app_guard`; agent sessions add `0017_agent_sessions` and `agent_guard`; settings adds
   `0018_settings_surface` and `settings_guard`; platform adds `0019_platform` and
   `platform_guard`; the identity-exit scenario adds `0020_identity_exit` and
   `identity_exit_guard`; token attenuation adds `0021_token_attenuation` and
   `token_attenuation_guard`. The operator copy remains deferred to the single
   wave-level migration pull request.
3. Set the operator's `GuardTable` to the new migration's table.
4. For an existing Lake, delete its `PostgresSchema` (`kubectl delete postgresschema
   lake-<name>-controldb-schema -n <ns>`); it is owned, so the operator recreates it within a
   reconcile and the new Job applies the file.

## Verify

```sh
kubectl get postgresschema -n <namespace>                              # APPLIED = VERSION, PHASE Applied
kubectl exec -n <namespace> lake-<name>-controldb-1 -c postgres -- psql -U postgres -d lake \
  -c "SELECT version FROM schema_migrations" \
  -c "SELECT conname FROM pg_constraint WHERE conrelid='customers'::regclass" \
  -c "SELECT version FROM schema_migrations WHERE version IN ('0007_identity_core', '0008_mfa_recovery', '0009_secrets', '0010_sso_federation', '0011_offline_recovery', '0012_forge_export_import', '0013_pki', '0014_metering', '0015_drive', '0016_auth_apps', '0017_agent_sessions', '0018_settings_surface', '0019_platform', '0020_identity_exit', '0021_token_attenuation', '0022_session_lookup')" \
  -c "SELECT to_regclass('session_lookup_guard')"
```

## Token attenuation migration

`0021_token_attenuation` adds `sessions.parent_session_id`, a self-referencing foreign key with
`ON DELETE CASCADE`, and an index for descendants. Authentication checks each ancestor’s live
session and device state; issuance stores an expiry no later than its parent. The migration
revokes pre-existing API/static tokens once because their ancestry cannot be recovered. Its
`schema_migrations` entry prevents a repeated schema application from revoking new tokens.

The deployment guard is `token_attenuation_guard`. The operator must vendor this updated schema
byte-for-byte, select that guard, and apply it before deploying the containing binary. The operator
copy is a separate repository change; this issue’s worktree contains only Lake. Verify with the
full PostgreSQL-backed static and shared suites and the token derivation tests described in
[`../api/identity.md`](../api/identity.md). When changing ancestry storage, keep both stores,
`Session`/`Principal`, and the common authenticated request context aligned.

## Session lookup migration

`0022_session_lookup` adds nullable `sessions.lookup_id uuid` and the unique B-tree index
`sessions_lookup_id_idx`. New credentials have a random public UUID plus a 256-bit random secret;
`secret_hash` holds SHA-256 of the complete versioned bearer, never the bearer itself. A selector
lookup returns at most one row before constant-time digest verification and live authorization
checks. The existing primary-key and parent indexes serve the derived token's ancestor checks.

Pre-migration rows have no lookup UUID and cannot be backfilled from their Argon2 hashes. The
migration revokes those rows once, guarded by its `schema_migrations` entry. Users sign in again
and recreate API/static tokens. Keeping old rows preserves history; their null lookup values are
never queried during authentication. Reapplying the schema does not revoke new credentials.
An older binary can still write a null lookup, but the new binary refuses it without scanning.
Coordinate migration and rollout so every serving replica understands the new format.

The new deployment guard is `session_lookup_guard`. Before deployment, the operator must vendor
the complete schema byte-for-byte, select this guard and apply the migration. That operator
change is deliberately outside this Lake worktree. Verification uses the full PostgreSQL-backed
static/shared suites, including `PostgresSessionAuthentication.*`; see
[authentication](../api/auth.md) and [measurement proof](../build/session-authentication.md).
Changes to the lookup or digest format must update this migration contract and those regressions.
