# Control-plane schema

**Source of truth: `db/schema.sql` in this repository.** The operator carries a byte-identical
vendored copy at `internal/controller/assets/lake-schema.sql` and applies it to every Lake's
Postgres. Change the schema here, then copy it into the operator in the same change; the
operator's test checks the copy carries the source-of-truth header.

`0088_repository_liquid_ids` (lake#805) adds nullable `repositories.liquid_id`, the id liquid gave
the repository (32 lowercase hex digits, what `liquid deploy` sends), with the partial index
`repositories_liquid_id_idx`; it is not unique, because a repository pushed from another's clone
shares that one's id. Lake fills it after each push and from the platform sync. Additive; its guard
is `repository_liquid_ids_guard`. The operator must vendor this schema and select that guard before
the binary rolls. See [platform.md](../api/platform.md#which-repository-a-row-names).

`0087_clerk_webhooks` (lake#765) adds `users.disabled_at` (set by a Clerk `user.deleted`; a disabled
account is issued no session and its sessions no longer resolve) and `clerk_webhook_deliveries`
(each applied `svix-id`, its event type and Clerk user). Its guard is `clerk_webhooks_guard`. The
operator must vendor this schema and select that guard. See [clerk.md](../api/clerk.md#webhooks).

`0086_identity_links` (lake#762, decision 65) lets an account replace its managed key with the
person's own identity: `custodial_identities.wrapped_seed` becomes nullable and gains
`seed_destroyed_at` (the seed is wiped, the public key kept, when the link commits);
`identity_link_challenges` holds a pending link (the verified own document, the statement to sign,
ten-minute expiry, one use); `identity_links` holds the committed link, one per account and one per
own identity, with both keys' signatures in `link_document`. Its guard is `identity_links_guard`.
The operator must vendor this schema byte-for-byte and select that guard before a binary with
`use_own_identity` serves; that handoff is separate. See [identity.md](../api/identity.md#your-own-identity).

`0085_network_tunnel_ca` adds public `client_ca_pem` (Lake-written) on `network_tunnel_grants`
and `relay_ca_pem` (operator-written) on `network_tunnel_grant_status`, both non-null text with
empty defaults. It leaves migration 0084 unchanged and adds no new guard. The operator must
vendor/apply this version before using the columns; that cross-repository handoff is separate.
See [network.md](network.md) and [tunnel credentials](../api/tunnel-grants.md).

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
| `name varchar(256) NOT NULL` | the handle: **one global namespace** shared by individuals and organizations (`customer_name_unique`); lowercase letters, digits, inner hyphens, 1–256 chars (`customer_name_format`), so uniqueness is effectively case-insensitive and a handle can never carry accounts or uppercase. No default: the application generates handles (`{adjective}-{noun}-{number}`) and must pre-check uniqueness to answer 409 rather than 500 |

**Repositories** (migration `0002_repositories`):

| `repositories` | |
|---|---|
| `id uuid` | v4 |
| `customer_id uuid NOT NULL` → `customers` | the owner; `ON DELETE CASCADE`; indexed |
| `name varchar(256) NOT NULL` | slug in the handle format (`repository_name_format`); partial-unique per customer while `deleted_at IS NULL`, so the live address is `<customer>/<name>` |
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
| `audit_retention` | One retention setting per customer, constrained to at least 400 days, timestamped on update; the application refuses to lower it. It governs the central log; a workspace's audit is kept until the workspace is deleted (migration `0063`, lake#318). |

Every capability call, sign-in and session event is one row, written by `audit::record` under a
transaction advisory lock that reads the latest digest and appends the next event; the signature
and digest chain make a missing or reordered row detectable to `verify_audit`. The provisional
`audit_events` shape that migration `0004` carried is gone: the store methods no longer write
their own rows, because `Registry::call` records every outcome.

## Repository lifecycle (migration `0006_repository_lifecycle`)

`repositories` gains nullable `deleted_at` and `retention_until` timestamps. Its visibility
constraint also accepts `organization`, between private and public. It also created
`repository_refs`, a copy of each liquid ref; migration `0064` drops it (lake#319), and its
`CREATE` is no longer in the schema. Lifecycle operations update
the repository and its policy-object parent in one store transaction; the registry records each
attempt in the signed audit stream.

Migration `0040_active_repository_names` replaces the original table uniqueness constraint with
`repository_name_unique` on `(customer_id, name) WHERE deleted_at IS NULL`. The retained tombstone
keeps its historical name, but a new live repository may use that name as soon as deletion commits.

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

The five `secrets_*` tables below are retired by `0057_secret_workspaces` (lake#328-#332): mounts,
secrets, versions, named keys and data keys live in the workspace Zetta of each mount's route, and
[`0070_drop_moved_tables`](#drop-moved-tables-migration) drops these tables, so a fresh database never has them. See
[0057](#secret-workspaces-migration-0057_secret_workspaces).

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
does not update `secrets_versions`. (Since `0057` the data keys are workspace records, and since
`0070` a rotation rewraps the workspace data keys and the PKI authorities only.)

## Secret workspaces (migration `0057_secret_workspaces`)

| Table or change | Purpose |
|---|---|
| `secret_mount_routes` | One row per mount: its account, owning organization (NULL when the account owns the mount, [`0071`](#secret-mount-owners-migration)) and workspace (`enforce_workspace_routing`: a live workspace of the same account, never changed). Content-free dispatch; no mount name, secret name, value or key. |
| `keystore.previous_wrapped_key`, `previous_version` | The customer KEK a rotation replaced, wrapped under its own version, kept until every workspace data key is rewrapped. |
| `secrets_dynamic_leases` | Loses its foreign key to `secrets_mounts`; a new mount has no row there (lake#362 moves leases). |
| `workspace_assignments` (`secrets_mounts`) | Each legacy mount's owner: `sole_workspace` or `needs_selection`. Its entries, versions, keys and data keys follow it. |
| `secret_workspaces_guard` | Guard table for migration `0057_secret_workspaces`. |

Nothing is dropped. Lake reads and writes secrets only in workspace records
([secrets.md](../api/secrets.md#where-secrets-live-lake328-332)). Until `0070` dropped them, a
customer KEK rotation also rewrapped the retired `secrets_data_keys` rows so the one-shot copy could
read them.

## Secret lease workspaces (migration `0058_secret_lease_workspaces`)

| Table or change | Purpose |
|---|---|
| `secret_lease_routes` | One row per dynamic lease (lake#362): its mount (foreign key to `secret_mount_routes`), account and workspace (`enforce_workspace_routing`: a live workspace of the same account, never changed), and `due_at`, when the expiry loop must next look at it (NULL once the lease is revoked or expired in its workspace; partial index `secret_lease_routes_due_idx`). Content-free dispatch; no role, value or ciphertext. |
| `secrets_dynamic_leases` | Retired: leases live in `lake_secrets_dynamic_leases` in their mount's workspace Zetta, and [`0070_drop_moved_tables`](#drop-moved-tables-migration) drops the table. The migration reports each lease whose mount has no route. |
| `secret_lease_workspaces_guard` | Guard table for migration `0058_secret_lease_workspaces`. |

Nothing is dropped. See [secrets.md](../api/secrets.md#where-leases-live-lake362).

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
| `forge_records` | Retired by `0054_forge_record_workspaces` (lake#341): forge records live in the workspace Zetta of their repository's workspace, and [`0070_drop_moved_tables`](#drop-moved-tables-migration) drops this table. It held the versioned, repository-owned issue, review, release, webhook, branch-protection, export, and GitHub-import facts. |
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
| `metering_usage` | Retired by `0053_metering_workspaces` (lake#348): repository transfer lives in the repository's workspace Zetta, and [`0070_drop_moved_tables`](#drop-moved-tables-migration) drops this table. It held one row per customer and UTC hour: a unique-stored-bytes gauge and transfer and compute counters. |
| `metering_build_records` | Retired by `0053_metering_workspaces` (lake#349): build records live in the repository's workspace Zetta. It was the customer/result idempotency ledger that added a build's rounded compute seconds to its hour once. |
| `metering_guard` | Guard table for this migration. |

Both data tables use the metering area prefix. Customer deletion cascades its usage and ledger.
The migration follows PKI migration `0013`.

## Settings quota (migration `0018_settings_surface`)

| Table | Purpose |
|---|---|
| `customer_storage_quotas` | Explicit per-customer storage quota override used by metering and repository-write enforcement. A zero value disables that customer's quota. |
| `customer_usage_limits` | An account's limit on one dimension of usage and the percentage it warns from ([`0072`](#usage-limits-migration), lake#564). Content-free; the usage itself stays in each workspace Zetta. |
| `settings_guard` | Guard table for the settings-surface migration. |

Customers without a row continue to use `LAKE_STORAGE_QUOTA_BYTES`. The operator schema copy is
deferred to the wave-level operator migration pull request.

## Drive sharing (migration `0015_drive`)

| Table | Purpose |
|---|---|
| `drive_share_links` | Retired by `0048_drive_share_link_workspaces` (lake#350): links live in the workspace Zetta that holds them, and [`0070_drop_moved_tables`](#drop-moved-tables-migration) drops this table. It held the folder object, stable liquid node id, path, role, HMAC token digest, expiry, creator, and optional revocation for a bearer link. |
| `drive_guard` | Guard table for this migration. |

Direct recipient shares remain ordinary `grants`; the link row adds expiry and revocation to a
synthetic `share-link:<id>` principal grant. Repository deletion cascades links. Tokens are never
stored. The migration follows metering migration `0014`.

## Application authentication (migration `0016_auth_apps`)

| Table | Purpose |
|---|---|
| `auth_applications` | Registration, isolated pool, exact origins and redirects, custom host, and theme. Split by `0059`: the row stays as content-free routing (id, account, pool, custom host, workspace), and the origins, redirects, name and theme live in the application's workspace Zetta. |
| `auth_app_signing_keys` | Per-application Ed25519 public keys and encrypted signing seeds, including rotation state. Retired by `0056`: each application's keys live in its workspace Zetta. |
| `auth_app_users` | Pool membership and active or suspended status for application users. Retired by `0055_application_user_workspaces` (lake#353): users live in the application's workspace Zetta, and [`0070_drop_moved_tables`](#drop-moved-tables-migration) drops this table. |
| `auth_app_organizations` | Application-facing organization profiles and metadata. Retired by `0055` (lake#354), with the users. |
| `auth_app_webhooks` | Event filters, destination, enabled state, and encrypted signing secret. Retired by `0056`: webhooks live in the application's workspace Zetta. |
| `auth_app_webhook_events` | Immutable lifecycle event payloads. Retired by `0049`: events live in the application's workspace Zetta. |
| `auth_app_webhook_deliveries` | Every delivery attempt, outcome, signature, and error. Retired by `0049`, with the events. |
| `auth_app_guard` | Guard table for this migration. |

[`0070_drop_moved_tables`](#drop-moved-tables-migration) drops the six `auth_app_*` tables above and
the four settings columns of `auth_applications` (`name`, `allowed_origins`, `redirect_uris`,
`theme`, with their checks), so a fresh database never has them; the routing row stays.

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

Both tables are retired by [`0060_platform_workspaces`](#platform-workspaces-migration-0060_platform_workspaces)
(lake#358, lake#359): lake no longer writes or reads them, and
[`0070_drop_moved_tables`](#drop-moved-tables-migration) drops them.

| Table | Purpose |
|---|---|
| `platform_environments` | Repository/name binding to the local environment policy object created when the mirror first sees an environment. |
| `platform_mirror` | Last operator-control JSON row Lake saw per `(kind, key)`, plus local `seen_at`; deployment, environment-value, and domain rows share this auditable cache. |
| `platform_guard` | Guard table for the platform migration. |

The issue brief reserved `0017_platform`, but `0017_agent_sessions` and `0018_settings_surface`
were already present on main. The platform tables therefore use the next collision-free migration,
`0019_platform`. Operator Zetta remains the source of truth; these tables provide stale reads and
local policy binding only.

## Platform workspaces (migration `0060_platform_workspaces`)

| Table or change | Purpose |
|---|---|
| `objects` (`environment`) | Each environment's policy object takes the environment's derived id, `md5(repository id, newline, name)` as a UUID, as its external id, unless another environment object already has it. The object keeps its id, parent and grants; lake finds it from the repository and the name. |
| `workspace_assignments` (`platform_environments`, `platform_mirror`) | Each row's owner: its repository's workspace (`repository_workspace`), `needs_selection` while the repository has none, `workspace_deleted` once that workspace is gone. A mirror row is named by `md5(kind, newline, key)`; one naming no repository is reported and not assigned. |
| `platform_workspaces_guard` | Guard table for migration `0060_platform_workspaces`. |

Nothing is dropped. Lake reads and writes environments and mirrored rows only in the repository's
workspace records ([platform.md](../api/platform.md#where-environments-live-lake358-lake359)); the
one-shot copy moves the retired rows.

## Audit workspaces (migration `0063_audit_workspaces`)

Decision 52 (lake#317, lake#318): `audit_events` keeps control-plane and identity events; events
about a workspace's data go to that workspace's own chain, `lake_audit_events` in its Zetta
([audit.md](../api/audit.md#which-log)). `audit_retention` stays and governs the central log.

| Table or change | Purpose |
|---|---|
| `workspace_assignments` (`audit_events`) | The workspace of each central event about a workspace's data, for the copy: an event of a workspace action that was not refused for want of authority, whose workspace its details or object name and that belongs to its account. `event_workspace`, or `workspace_deleted` (it stays central). Every other event stays central, without a row; the migration reports the counts. |
| `audit_workspaces_guard` | Guard table for migration `0063_audit_workspaces`. |

Nothing is dropped and `audit_events` stays append-only; the one-shot copy puts each assigned event
in its workspace as it is. `0070` leaves audit alone: the split copied events rather than moving a
table, and the central chain keeps every row so it still verifies.

## Repository records (migration `0064_repository_records`)

Decision 54 (S10, S11). The `repositories` row stays as routing; what a person wrote about a
repository moves to its workspace, and the unused copy of liquid's refs goes
([repositories.md](../api/repositories.md#where-the-description-lives-lake315-part-2)).

| Table or change | Purpose |
|---|---|
| `repository_refs` | **Dropped** (lake#319). A copy of each liquid ref, written after a push, a replication and a `refs` read, that nothing read; the liquid repository is the authority. Its `CREATE` (0006) is gone, so a rerun never recreates it. Nothing is copied. |
| `repositories.description` | No longer written or read by lake (lake#315 part 2): the description is the repository's sealed record in `lake_repositories` of its workspace Zetta. New rows leave it `''`. Dropped by [`0070_drop_moved_tables`](#drop-moved-tables-migration). |
| `workspace_assignments` (`repository_descriptions`) | The owner of each repository that holds a description, for the copy: `repository_workspace`, `needs_selection` while it has no workspace (resolved by the copy once it has one; `0066`, lake#513, assigns it), or `workspace_deleted`. Unassigned rows are reported with `RAISE WARNING`. |
| `repository_records_guard` | Guard table for migration `0064_repository_records`. |

The operator's vendored schema test lists `CREATE TABLE IF NOT EXISTS repository_refs` among the
statements the schema must contain; that entry goes when the operator vendors this schema.

## Workspace deletions (migration `0065_workspace_deletions`)

Decision 54 S12 (lake#514): deleting a workspace cascades to its repositories, apps, unfinished
builds and tenant images ([workspaces.md](../api/workspaces.md#deletion)).

| Table or change | Purpose |
|---|---|
| `workspace_deletions` | One row per workspace deleted since this migration, written in the transaction that sets the workspace's desired state to `deleted`: `customer_id`, `workspace_name`, `requested_by`, `requested_at`, the destroyed `repositories`, `apps`, `builds` and `images` (JSON arrays of `{id, name, state}`), and `completed_at`, set once lake has removed the tenant image tags and search segments and recorded the central audit events. A row without `completed_at` is resumed by lake's owner loop. |
| `workspace_deletions_pending_idx` | The deletions lake has not completed, oldest first. |
| `workspace_deletions_guard` | Guard table for migration `0065_workspace_deletions`. |

The operator does not read the table: it still deletes a workspace's Zetta from `workspaces.desired`.
A workspace deleted before the migration has no row, and repeating `delete_workspace` for it is a
`conflict`, as before.

## Tenant image routes (migration `0068_oci_repository_routes`)

Decision 54 S4-S6 (lake#383 phase A): the registry splits. A tenant image repository belongs to one
workspace, whose Zetta holds its manifests and tags as sealed records (`lake_oci_manifests`, keyed by
digest and written `if_absent`; `lake_oci_tags`, keyed by repository and tag and moved at the
revision read). Image addresses do not change: lake resolves this route before it reads any
workspace ([registry.md](../api/registry.md#where-a-tenant-image-lives)). `0067` is held by the open
lake#380 change.

| Table or change | Purpose |
|---|---|
| `oci_repository_routes` | `(customer_id, name)` → `workspace_id`, with `created_at` and `updated_at`. `name` is the path below the account: `<image>` or `cache/<image>` only, checked against OCI's lowercase grammar, with `<image>` at most 190 characters. No manifest, digest or tag. Written by a build's first push of its own target (from the build's workspace) or by `create_image_repository` (an explicit `workspace_id`); there is no default workspace. Deleted for a workspace in the `0065` cascade transaction, or by the operator's `unroute_image_repository`, which removes or moves `<image>` with `cache/<image>` in one transaction. |
| `oci_repository_routes_workspace_idx` | A workspace's routes, for the deletion cascade and listings. |
| `enforce_oci_repository_route` (trigger `oci_repository_route_routing`) | Refuses any change of a route's account, name or workspace, and a route to a workspace that is not live in the account (`23514`). |
| Backfill | Once: each image name the apps and builds of exactly one live workspace of the account use gets a route there, for `<name>` and `cache/<name>`; a name several workspaces use is reported with `RAISE WARNING`. The registry's files are not visible to SQL: the one-shot copy (`copy_workspace_records`, collection `oci_tags`) routes the repositories it finds by the same rule, then by the account's only live workspace, and reports the rest. |
| `oci_repository_routes_guard` | Guard table for migration `0068_oci_repository_routes`. |

npm, Python, debuginfod and one-component OCI repositories stay with the instance and have no
route. Blobs stay in the instance registry until phase B (lake#380). The operator never reads the
table; its vendored schema rehearsal needs the new guard.

## Workspace volumes (migration `0069_workspace_volumes`)

Decision 54 S13 (operator#259, operator#260, lake#380): after the move a workspace Zetta's volume
holds the workspace's liquid store, tenant images and search index, and on a one-node workspace it is
the only copy. Lake writes desired state; the operator grows each workspace Zetta's claims, takes CSI
snapshots and reports back here. It never talks to a workspace Zetta (decision 51). The
capabilities are in [volumes.md](../api/volumes.md). `0067` is held by the open lake#380 change.

| Table or change | Purpose |
|---|---|
| `workspaces.storage_gi` | Desired size of each of the workspace Zetta's volumes, in GiB: integer, default 5, 1 to 32767. Set by `set_workspace_storage`, which advances `generation`. |
| `enforce_workspace_storage_growth` (trigger `workspace_storage_growth`) | Refuses any update that lowers `storage_gi` (`23514`): an online-expanded block volume cannot shrink. |
| `workspaces.storage_gi_observed`, `storage_phase`, `storage_message` | Written by the operator with the workspace's phase, in the same generation-guarded update: the smallest claim capacity it sees in GiB, `Pending` (no claim yet), `Resizing`, `Ready` or `Failed` (for example a storage class without expansion), and why. |
| `volume_snapshot_policies` | Content-free desired state, one row per target: `target` (`store`, lake's own store volume, or a workspace id), `schedule` (five cron fields in UTC, default `0 3 * * *`), `keep` (1 to 100, default 7), `requested_at` (a one-off request the operator honors when it is newer than the target's last snapshot), `created_at`, `updated_at`. No foreign key to `workspaces`. |
| `add_workspace_snapshot_policy` (trigger `workspace_snapshot_policy`) | Gives every new workspace the default policy. |
| `notify_lake_volume_snapshots` (trigger `lake_volume_snapshots_changed`) | `NOTIFY lake_volume_snapshots` with the target on every policy insert or update, so the operator takes a requested snapshot without waiting for its poll. |
| `volume_snapshot_status` | Written only by the operator, one row per target: `snapshot_name` (its last `VolumeSnapshot`, `<claim>-<UTC timestamp>`), `ready_to_use`, `snapshot_at`, `message`, `updated_at`. |
| Backfill | Once: a policy for `store` and for every workspace that is not deleted. |
| `workspace_volumes_guard` | Guard table for migration `0069_workspace_volumes`. |

A deleted workspace's policy and status rows stay, as its snapshots do: the operator's
`VolumeSnapshotClass` retains their contents (decision 54 S12), and removing them is a separate,
explicit action. `move_workspace_store` does not yet wait for a snapshot taken after the request
(lake#380 wires that check once its change merges). The operator selects the new guard and reads
these tables with the statements `tests/test_operator_contract.cpp` copies.

## Identity exit scenario (migration `0020_identity_exit`)

| Table or change | Purpose |
|---|---|
| `identity_taap_assertions` | One-use digest ledger binding each accepted liquid-key sign-in assertion to its lake user and pool. |
| `auth_app_authorization_codes` | Five-minute application code hashes with exact redirect, PKCE S256 challenge, nonce, user, and one-use timestamp. Retired by `0061` (lake#363): codes are issued and redeemed in the application's workspace Zetta, and [`0070_drop_moved_tables`](#drop-moved-tables-migration) drops this table. |
| `pki_certificates.revoked_at` and `agent` profile | Immediate agent-certificate status plus rotation linkage through the existing `renewed_from_id`. |
| `secrets_dynamic_leases` | Encrypted generated credential, role, bounded expiry, revocation, and renewal count for each mount lease. Since 0057 the value is sealed with the mount data key read from the mount's workspace; retired by `0058_secret_lease_workspaces` (lake#362), when leases moved into that workspace, and dropped by `0070`. |
| `identity_exit_guard` | Guard table for migration `0020_identity_exit`. |

Dynamic-secret ciphertext uses the existing mount/customer key hierarchy. Authorization codes and
TAAP assertions retain hashes rather than bearer values. Certificate rows retain only public
certificate output and lifecycle metadata.

## How it is applied

With `spec.postgres` on, the operator owns, per Lake, a ConfigMap `lake-<name>-controldb-migrations`
(the SQL) and a `PostgresSchema` `lake-<name>-controldb-schema` whose controller runs an idempotent
Job: wait for the DB, skip if the guard table exists, apply the whole file in **one transaction**
with `psql --single-transaction`. **The guard is the table the newest migration creates**
(`app_forward_auth_guard` in the source tree), so a new migration means a new guard and the next
Job re-runs the idempotent file, adding only what is missing. The version is a content hash of the SQL
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
   `token_attenuation_guard`; session lookup adds `0022_session_lookup` and
   `session_lookup_guard`; Clerk sign-in adds `0023_custodial_identities` and
   `custodial_identity_guard`; managed services add `0024_services` and `services_guard`; the service catalog adds
   `0025_service_catalog` and `service_catalog_guard`; Ship apps add `0034_web_apps`, then
   `0035_web_apps_operator_contract` and
   `web_apps_operator_contract_guard`. The operator copy remains deferred to the single
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
  -c "SELECT version FROM schema_migrations WHERE version IN ('0007_identity_core', '0008_mfa_recovery', '0009_secrets', '0010_sso_federation', '0011_offline_recovery', '0012_forge_export_import', '0013_pki', '0014_metering', '0015_drive', '0016_auth_apps', '0017_agent_sessions', '0018_settings_surface', '0019_platform', '0020_identity_exit', '0021_token_attenuation', '0022_session_lookup', '0023_custodial_identities', '0024_services', '0025_service_catalog', '0034_web_apps', '0035_web_apps_operator_contract', '0040_active_repository_names', '0041_app_forward_auth')" \
  -c "SELECT to_regclass('app_forward_auth_guard')"
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

## Custodial identity migration

`0023_custodial_identities` adds `custodial_identities`, one row per user for whom lake holds a TAAP
identity: `user_id` (primary key, cascading from `users`), the unique `taap_id`, the base64
Ed25519 `public_key`, and `wrapped_seed`, the seed wrapped under `LAKE_INSTANCE_KEK` exactly as
`lake_identity.wrapped_seed` is. A row is written only when a person first signs in through Clerk
and no lake account matches them; the user's `users.taap_id` is the same identifier. See
[`../api/clerk.md`](../api/clerk.md).

The migration is additive and needs no backfill. The new deployment guard is
`custodial_identity_guard`. The operator must vendor this schema byte-for-byte and select that
guard before the binary that writes the table is deployed; an older binary never reads it.

## Managed services migration

`0024_services` adds `services`, written by lake, and `service_status`, written by the Orama
operator: the desired and observed state of each customer's Zetta and Postgres clusters. Partial
unique indexes keep one live service per name and one live default workspace per customer; `slug`
is unique for the life of the Lake. The contract, including which side writes which column, is
[`services.md`](services.md).

The migration is additive and needs no backfill: an existing decision-45 workspace is recorded the
next time its customer starts it, and the operator adopts the cluster. The new deployment guard is
`services_guard`. The operator must vendor this schema and select that guard before the binary that
reads the tables is deployed.

## Service catalog migration

`0025_service_catalog` replaces `service_kind_check` so `services.kind` also accepts `kafka`,
`clickhouse`, `tidb`, `cassandra` and `valkey` (liquid-bridge decision 47). Dropping and re-adding
the constraint is idempotent, and every existing row satisfies the wider list. The guard is
`service_catalog_guard`; the operator vendors this schema and selects it before a binary that
creates the new kinds is deployed.

## Analytics services migration

`0027_analytics_services` widens `service_kind_check` again for `duckdb` and `polars` (lake#195),
each served by a wrapper service of ours. Idempotent; guard `analytics_services_guard`. The operator
vendors it and selects that guard before a binary that offers the kinds is deployed.

## Stores migration

`0028_stores` adds `stores` (written by lake: type, provider, name, `st-` slug, `spec` as
`{size_gi, region, public}`, desired and generation) and `store_status` (written by the operator),
with one live name per customer (lake#196). Idempotent (`IF NOT EXISTS`); guard `stores_guard`.
The operator vendors it (orama-operator#184) and selects that guard before a binary that offers
storage is deployed. Contract: [`storage.md`](storage.md); capabilities and page:
[`../api/storage.md`](../api/storage.md).

## Non-expiring credentials migration

`0029_non_expiring_credentials` adds `customers.allow_non_expiring_credentials` (the customer's
opt-in, `false` by default) and `agent_sessions.revoked_at` (lake#234). A never-expiring agent key
stores `expires_at = 'infinity'`, which the existing `agent_sessions_expiry_check` accepts, and
reads back as `expires_at: "never"`. Idempotent (`ADD COLUMN IF NOT EXISTS`); guard
`credentials_guard`. The operator vendors it and selects that guard before a binary that offers
never-expiring credentials is deployed. Capabilities: [`../api/agents.md`](../api/agents.md) and
[`../api/identity.md`](../api/identity.md).

## Agent launches migration

`0030_agent_launches` adds `agent_launches` (written by lake: agent, mode, input, scopes, `al-`
slug, desired, trigger, who launched it), `agent_launch_status` (written by the operator: phase,
message, endpoint, exit code, result and the **SHA-256** of the pod's credential, never the
credential), `agent_schedules` (one per agent, its next run) and `ask_messages` (Ask Orama
conversations, read back only by the person who had them; retired by `0045`, below, when
conversations moved into workspace Zettas, and dropped by `0050`, whose rerun-safe schema no longer
creates it). Idempotent; guard `agents_guard`. The
operator vendors it and selects that guard before a binary that launches agents is deployed.
Contract and capabilities: [`../api/agent-launches.md`](../api/agent-launches.md).

## Agent tasks migration

`0032_agent_tasks` adds `agent_tasks` (written by lake: a library agent's turn to run in the
background, with its body, scopes, deadline and when it was last published on the agent bus),
`agent_task_status` (written by lake when an agent claims the task and as it reports: phase, message,
answer and the **SHA-256** of the task's credential, cleared when the task finishes) and `opportunities` (Design → Opportunities:
what Auto-Suggest Agent recommended, kept across scans, with what the person did with each; retired by
`0046`, below, when Opportunities moved into workspace Zettas). Idempotent;
guard `agent_tasks_guard`. Contract: [`../api/agent-bus.md`](../api/agent-bus.md). Since `0062`
(below) the body, message and answer live in the task's workspace Zetta, and both tables are
content-free dispatch rows.

## Agent task events migration

`0033_agent_task_events` adds `agent_task_status.events_from` (bigint, default 0): the `agent-events`
sequence lake's bus loop had reached when the agent claimed the task, at or before the task's first
event, so following a task reads from there (lake#275). Idempotent; guard `agent_task_events_guard`.
Contract: [`../api/agent-bus.md`](../api/agent-bus.md).

## Agent cron schedules migration

`0031_agent_cron_schedules` adds `agent_cron_schedules` (written by lake: any number per agent,
each with its five-field `cron`, `time_zone`, `input`, scopes, `as-` slug, desired and generation),
`agent_cron_schedule_status` (written by the operator: phase, message, last firing and the
**SHA-256** of the schedule's credential) and `agent_cron_runs` (written by the operator: one row
per Job each schedule's CronJob started). It retires `agent_schedules` without dropping it: lake no
longer reads it, and dropping it is a separate, deliberate change. Idempotent; guard
`agent_cron_guard`. The operator vendors it and selects that guard before a binary that schedules
through CronJobs is deployed. Contract: [`../api/agent-launches.md`](../api/agent-launches.md).

## Ship apps migration

`0034_web_apps` adds the `apps` desired-and-observed row shared by lake and the Orama operator.
Lake writes deployment, runtime, bindings, environment, lifecycle and generation fields; the
operator writes phase, URL, readiness, message and observed generation. Desired changes notify
`lake_apps`. Migration `0035_web_apps_operator_contract` aligns runtime/image modes and deleted
intent, adds `source_entrypoint` and operator-written `token_hashes_json`, normalizes bindings and
removes the earlier credential table. Only hashes remain; authentication enforces source,
workspace and event purpose restrictions. Idempotent; guard `web_apps_operator_contract_guard`.
The complete ownership, quota and reconciliation contract is [`web-apps.md`](web-apps.md).

## In-cluster builds migration

`0036_builds` adds the build desired-and-observed row shared by Lake and the Orama operator. Lake
writes repository, revision, context, Dockerfile, target, allowed build arguments and queued or
cancelled intent. The operator writes phase, digest, size, message and only SHA-256 hashes for its
source, push and event credentials. Desired changes notify `lake_builds`. Idempotent; guard
`builds_guard`. The ownership and Job handoff contract is [`builds.md`](builds.md).

## Streams migration

`0037_streams` records topic creators in `stream_owners`, keyed by customer, workspace service
incarnation and concrete topic name. Idempotent; its guard is `streams_guard`. Since lake#379 the
creators live in each topic's workspace Zetta, and `0047_stream_owner_workspaces` records each
row's workspace in `workspace_assignments` for the operator's one-shot copy; it adds no column and
drops nothing (guard `stream_owner_workspaces_guard`). Apply it
before the Streams binary and vendor it in the operator rollout. Ownership failure behavior and
account-scoped bus views are specified in [`streams.md`](streams.md).

## Workspace routing migration

`0038_workspace_data_plane` adds immutable workspace service IDs to tasks, apps and builds.
`workspace_assignments` records sole-default legacy assignments and unresolved object/customer
IDs; no event payloads are added. Inserts require a live Zetta in the same customer and updates
cannot change workspace/customer identity. Migration `0039_workspaces` replaces the service
foreign keys with `workspaces(id)`, records legacy naming remaps, and adds workspace desired
state with notifications on every change. This migration's guard is `workspaces_guard`.
See [the workspace contract](workspaces.md) for the operator handoff and migration details.
See the [workspace data-plane design](../architecture/workspace-data-plane.md) for remediation,
cutover, retention and the complete storage audit.

## Active repository names migration

`0040_active_repository_names` drops the original `repository_name_unique` table constraint and
recreates that name as a partial unique index over `(customer_id, name) WHERE deleted_at IS NULL`.
No backfill is required because the earlier constraint prevented conflicting historical rows.
The migration is idempotent and records `repository_names_guard` for operator schema application.

The related soft-deletion models already release names: `apps_customer_name_idx` excludes
`desired = 'deleted'`, `services_customer_name_idx` and `stores_customer_name_idx` include only
`desired = 'present'`, and `workspaces_name_idx` excludes `desired = 'deleted'`. Secret-version
soft deletion is version history rather than a reusable name namespace, so its `(entry_id,
version)` key remains unchanged.

## Protected app forward-auth migration

`0041_app_forward_auth` adds `app_authorization_codes`, Lake-owned dispatch state for a browser
handoff from Lake's host to a protected app host. Rows contain only a SHA-256 code hash, app ID,
user ID, exact app host, 60-second expiry, and optional use time. The app and user foreign keys
cascade on deletion. Exchange marks one live row used atomically while rechecking the live app,
host, workspace, and membership. The expiry index supports the minute sweep of expired and spent
rows. The idempotent deployment marker is `app_forward_auth_guard`.

## Ask message workspaces migration

`0045_ask_message_workspaces` records, in `workspace_assignments`, the workspace that owns each
retired `ask_messages` row: the account's only live workspace (`sole_workspace`), or
`needs_selection`; a row without an account is reported by a warning and listed by the copy as
`no_customer`. Ask Orama conversations live in the owning workspace Zetta (lake#370), and lake no
longer writes or reads the table except through the operator-only `copy_workspace_records`, which
reapplies the same assignment before each run. No column is added and nothing is dropped; the
table is dropped by `0050_drop_ask_messages`, below, and this block now runs only while it exists. Idempotent; guard
`ask_message_workspaces_guard`. The operator vendors it and selects that guard before the binary
is deployed. Mechanism and cutover: [`../architecture/workspace-records.md`](../architecture/workspace-records.md).

## Opportunity workspaces migration

`0046_opportunity_workspaces` records, in `workspace_assignments`, the workspace that owns each
retired `opportunities` row: the workspace the scan's agent task names (`task_workspace`); for a
task written before tasks named their workspace, the account's only live workspace
(`sole_workspace`); otherwise `needs_selection`. A row whose scan ran in a workspace that has since
been deleted is recorded as `deleted_workspace` without a workspace: it went with that workspace
and is never moved into another. Each unassigned row is reported by a warning. Design →
Opportunities live in the owning workspace Zetta (lake#376), and lake no longer writes or reads the
table except through the operator-only `copy_workspace_records`, which reapplies the same
assignment before each run. No column is added and nothing is dropped. Idempotent; guard
`opportunity_workspaces_guard`. The operator vendors it and selects that guard before the binary is
deployed. Mechanism and cutover: [`../architecture/workspace-records.md`](../architecture/workspace-records.md).

## Drive share link workspaces migration

`0048_drive_share_link_workspaces` records, in `workspace_assignments`, the workspace that owns
each retired `drive_share_links` row: the only live workspace of its repository's account
(`sole_workspace`), or `needs_selection`. It creates `drive_share_link_routes` (`token_hash`,
`link_id`, `workspace_id`, `recorded_at`) and fills it for every assigned row. That table is
dispatch: it lets a bearer token issued before lake#350, which does not name its workspace, find the
workspace Zetta that holds its record, and holds no path, role, expiry or creator. A route is
removed with its workspace (`ON DELETE CASCADE`). Links now live in the workspace Zetta's
`lake_drive_share_links` collection (lake#350); a new token is `<workspace id>.<link id>.<secret>`
and needs no route. Lake no longer writes or reads `drive_share_links` except through the
operator-only `copy_workspace_records`, which reapplies the assignment and routes before each run.
Nothing is dropped. Idempotent; guard `drive_share_link_workspaces_guard`. See
[`../api/sharing.md`](../api/sharing.md#where-links-live).

## Application webhook workspaces migration

`0049_application_webhook_workspaces` (lake#356, lake#357) adds `auth_applications.workspace_id`,
the workspace whose Zetta holds the application's lifecycle events and delivery attempts. It is
content-free routing, like `stores.workspace_id`. The trigger `application_workspace_routing`
(`enforce_application_workspace`) requires a live workspace of the application's own account on
insert, accepts one assignment of a legacy application that has none, and refuses any other change
of the workspace or the account. An application whose account has exactly one live workspace is
assigned it (`sole_workspace`); every other is recorded as `needs_selection` with a warning. Each
retired `auth_app_webhook_events` row's owner is recorded as its application's workspace
(`application_workspace`) or `needs_selection`; a delivery attempt always goes where its event
goes and has no row of its own. Lake no longer writes or reads either table except through the
operator-only `copy_workspace_records`, which reapplies the assignment and resolves events whose
application has since been assigned. Nothing is dropped. Idempotent; guard
`application_webhook_workspaces_guard`. Mechanism and cutover:
[`../architecture/workspace-records.md`](../architecture/workspace-records.md#cutover-runbook-application-events-and-deliveries-dev-cluster).

## Drop ask messages migration

`0050_drop_ask_messages` (lake#483) drops the retired `ask_messages` table. Ask Orama conversations
live in each workspace Zetta's `lake_ask_messages` collection (lake#370), and the operator's copy
was verified on dev before the drop: 15 of 15 rows, `complete: true`. The `workspace_assignments`
rows `0045` recorded for the table stay as the record of where each row went. Because every schema
Job reapplies this file from the top, the table's original `CREATE` (from `0030`) is removed and
`0045`'s block runs only while the table exists, so a rerun never recreates it. Lake has no reader
of the table left, and `copy_workspace_records` no longer accepts `ask_messages`. Idempotent; guard
`drop_ask_messages_guard`. The operator vendors it and selects that guard.

## Drop moved tables migration

`0070_drop_moved_tables` (lake#311, decision 54) drops every retired copy of workspace data whose
one-shot copy was reported run and verified on dev: the tables `opportunities`, `stream_owners`,
`drive_share_links`, `auth_app_webhook_deliveries`, `auth_app_webhook_events`, `auth_app_webhooks`,
`auth_app_signing_keys`, `auth_app_organizations`, `auth_app_users`,
`auth_app_authorization_codes`, `metering_usage`, `metering_build_records`, `forge_records`,
`secrets_dynamic_leases`, `secrets_versions`, `secrets_entries`, `secrets_encryption_keys`,
`secrets_data_keys`, `secrets_mounts`, `platform_mirror` and `platform_environments`; and the
columns `auth_applications.name`, `allowed_origins`, `redirect_uris`, `theme` (with
`auth_applications_settings_check`), `agent_tasks.body`, `agent_task_status.message` and `result`,
and `repositories.description`. Each is dropped with `DROP TABLE IF EXISTS` or
`DROP COLUMN IF EXISTS`.

Before dropping anything, one block refuses rows with no recorded workspace owner: it counts, per
table, the rows still present with no `workspace_assignments` row naming a workspace, unless the
outcome says the workspace was deleted (`workspace_deleted`, or `deleted_workspace` as `0046`
spelled it); for the secrets tables, a mount without a route. Mirror rows naming no repository,
expired codes and usage rows without transfer are not counted. If any table has one, it raises
`0070_drop_moved_tables: rows without a recorded workspace owner: <table>=<count>, ...` and the
schema transaction rolls back with nothing dropped. The check proves an owner was recorded, not that
a copy ran: `copy_workspace_records` `plan`, `copy` and `verify` are a manual gate before applying.
When it refuses, nothing is dropped. Either run `copy_workspace_records` `plan`, `copy` and `verify` for each table named with the previous lake image (`oramasearch/lake:20261005a`, the image on dev when this was written, or any image built before `0070`), resolve its unresolved rows and apply the schema again; or, on a development environment and only with the owner's approval (it destroys data), delete the named rows and apply again.

What stays: the routing rows (`auth_applications`, `repositories`, `agent_tasks`,
`agent_task_status`), `drive_share_link_routes`, `secret_mount_routes`, `secret_lease_routes`, every
policy object (environments, secret mounts and secrets), `keystore`, `audit_events` and
`audit_retention` (decision 52: nothing of the central chain is removed), agent sessions and SSO
logs (decision 53), the operator's desired state (decision 51), and the `workspace_assignments` rows
the earlier migrations recorded. The tables' original `CREATE`s and the dropped columns are removed
from the earlier sections, and the earlier migration blocks that read them run only while they
exist, so a fresh database never has them and a rerun never recreates them. Lake has no reader of
any of them left; `copy_workspace_records` accepts only `audit_events` and `oci_tags`. Idempotent;
guard `drop_moved_tables_guard`.

The operator vendors this schema and selects `drop_moved_tables_guard`. Its
`TestLakeSchemaMatchesVendoredSource` lists `CREATE TABLE IF NOT EXISTS secrets_mounts`,
`secrets_versions`, `secrets_encryption_keys` and `secrets_data_keys` among the statements the
schema must contain; those four entries go, and its Postgres rehearsal query needs the new guard.

## Secret mount owners migration

`0071_secret_mount_owners` (lake#561) lets an account own a secret mount. A mount created without
`organization_id` belongs to the customer that owns its workspace (an individual's personal account,
or an organization's account), so its route has no organization: the migration drops `NOT NULL` from
`secret_mount_routes.organization_id`, and such a route stores NULL there. Its policy object hangs
under the customer's (`customer → environment secrets/<customer> → secret_mount`); an organization's
mount keeps `organization → environment secrets/<customer>/<organization> → secret_mount`. The route
stays content-free. Existing rows are untouched, nothing is dropped, and the statement is
idempotent; guard `secret_mount_owners_guard`. The operator vendors the schema and selects that
guard in its own pull request before a lake with this change is rolled. See
[`../api/secrets.md`](../api/secrets.md#who-owns-a-mount-lake561).

## Usage limits migration

`0072_usage_limits` (lake#564) adds `customer_usage_limits` (customer, `dimension`, `limit_value`,
`warn_percent` from 1 to 100, default 80, `updated_at`; primary key customer and dimension): an
account's limit on one dimension of usage, in that dimension's unit
([`../api/metering.md`](../api/metering.md#limits)). A counted dimension's limit is per UTC calendar
month; storage's row only carries its warning percentage, since its limit is the enforced
`customer_storage_quotas` quota. It is content-free account configuration beside that quota: the
usage it is compared with stays in each workspace Zetta (`lake_usage`, `lake_storage`). Prices,
plans and bills are billing's own tables ([billing migration](#billing-migration)). Nothing is
dropped; idempotent; guard `usage_limits_guard`. The operator vendors the schema and selects that
guard in its own pull request before a lake with this change is rolled.

## Billing migration

`0073_billing` (lake#564, decision 56) adds billing records, in the control plane as multitenancy
([`../api/billing.md`](../api/billing.md)). Since lake#635 (usage only, no migration) each
publication of the price list is one `billing_plans` row, `usage-<YYYYMM>-<publish µs>`, with no fee
and nothing included ([how the list is stored](../api/billing.md#how-the-list-is-stored)):

- `billing_plans` (`id` text slug, `name`, `description`, three-letter `currency`, `base_micros`
  monthly fee, `active`, `position`, timestamps) and `billing_plan_prices` (plan, `dimension`,
  `unit_micros` for every `per` units beyond `included` units a month; primary key plan and
  dimension, cascading with the plan);
- `billing_subscriptions` (customer, `effective_from` on the first of a month, enforced by a check,
  `plan_id`; primary key customer and `effective_from`; cascading with the customer). Unused since
  lake#635: lake is priced by usage only and there is no plan to choose. The table stays, empty;
  dropping it is not worth a migration;
- `billing_invoices` (uuid `id`, `number` from a sequence, customer, `period` `YYYY-MM`, plan id and
  name, currency, `total_micros`, `state` `due`/`paid`/`void`, `provider`, `provider_reference`,
  `instructions`, `note`, issued, paid and voided times) with a partial unique index allowing one
  invoice a customer and month that is not void. `customer_id` is not a foreign key: an invoice is
  a financial record and outlives its account;
- `billing_invoice_lines` (invoice, `position`, `kind` `plan`/`usage`, `dimension`, `description`,
  `unit`, `quantity`, `included`, `billable`, `unit_micros`, `per`, `amount_micros`), cascading with
  the invoice.

The quantities are the only usage that reaches PostgreSQL: one number per dimension per invoiced
month, frozen when the invoice is issued. The usage itself stays in each workspace Zetta. Lake
writes these tables only where `LAKE_BILLING` is on; elsewhere they stay empty. Nothing is dropped;
idempotent; guard `billing_guard`. The operator vendors the schema and selects that guard in its own
pull request before a lake with this change is rolled.

## Payment provider migration

`0074_payment_provider` (lake#620, decision 57) adds what Stripe needs
([`../api/billing.md#stripe`](../api/billing.md#stripe)):

- `billing_accounts` (customer, `provider`, `provider_customer_id`, created; primary key customer and
  provider; cascading with the customer): the account's billing row, holding its Stripe customer id
  (`cus_...`). Written once; a second write keeps the first id;
- `billing_invoices.payment_url` (Stripe's hosted invoice page, at most 2048 characters) and
  `provider_status` (`open`, `paid`, `payment_failed`, `void`), both empty for the manual provider,
  and the partial index `billing_invoices_by_provider_reference` on (`provider`,
  `provider_reference`) for the webhook's lookup;
- `billing_payment_events` (`provider`, `event_id`, `event_type`, `reference`, `invoice_id`,
  received; primary key provider and event id): every provider event applied, written in the
  transaction that moves the invoice, so a second delivery changes nothing.

No key, card or workspace content is kept: the Stripe keys are lake's own platform credentials,
environment variables of the hosting platform (lake#628). Nothing is dropped; idempotent; guard `payment_provider_guard`.
The operator vendors the schema and selects that guard in its own pull request before a lake with
this change is rolled.

## Workload resources migration

`0075_workload_resources` (lake#643) prices compute by the resources each workload is scheduled
with ([`../api/metering.md#resources`](../api/metering.md#resources)):

- `workload_resources` (`object_kind` `workspace`, `service`, `store`, `app`, `build` or
  `agent_launch`; `object_id`; `pods`; `cpu_millis`; `memory_bytes`; `gpus`; `gpu_class`;
  `vram_bytes`; `observed_at`; primary key kind and id), written only by the Orama operator: the
  totals of a workload's scheduled pods, each container's request or, where it has none, its
  limit. `pods` is how many are scheduled now; a build's or a job's row keeps what its pods ran
  with after they finish, with `pods` 0. Numbers only. Lake's owner loop reads it and records
  CPU, RAM, GPU and VRAM seconds in each workspace Zetta; nothing about usage is kept here;
- `billing_plan_prices.class` and `billing_invoice_lines.class` (a GPU class such as `h100`, empty
  for a dimension's default rate), with the price's primary key now (`plan_id`, `dimension`,
  `class`), so the price list can carry one GPU rate per class.

Nothing is dropped; idempotent (the primary key is dropped and added again on every run); guard
`workload_resources_guard`. The operator vendors the schema, selects that guard and fills the table
in its own pull request ([orama-operator#288](https://github.com/oramasearch/orama-operator/pull/288))
before a lake with this change is rolled. `OperatorContract.*` prepares that pull request's
statements against this schema.

## Organization deletion migration

`0076_organization_deletion` (lake#645) lets an organization be deleted
([`../api/organizations.md#deleting-an-organization`](../api/organizations.md#deleting-an-organization)):

- `organizations.deleted_at` and `customers.deleted_at` (null while live). `delete_organization`
  removes the organization's teams, memberships, invitations, SSO providers, federation issuers,
  recovery attempts, secret mount and lease routes, and the policy objects of the organization
  and its account with every grant on them, then sets both. The rows stay, marked, because
  `audit_events.customer_id` is `ON DELETE RESTRICT` and an audit event is never rewritten; every
  read and list of organizations and customers leaves a marked row out.

Nothing is dropped; idempotent; guard `organization_deletion_guard`. The operator vendors the schema
and selects that guard before a lake with this change is rolled; a lake before it never sets the
columns, and a lake with it on an older schema fails its organization and customer reads, so the
schema goes first.

## Metered jobs migration

`0077_metered_jobs` (lake#658) widens `workload_resources.object_kind` to `agent_cron_run`: one row
per run an agent cron schedule's CronJob starts (`object_id` the `agent_cron_runs` id), reported by
the Orama operator from the run's pods (or its Job's pod template) and kept with `pods` 0 after they
finish, as for a job-mode launch. Lake charges each finished build, job launch and cron run once,
recording that it did in the run's workspace Zetta (`lake_metering_job_records`,
[`../api/metering.md#resources`](../api/metering.md#resources)); nothing about usage is kept here.

Nothing is dropped; idempotent (the check is dropped and added again on every run); guard
`metered_jobs_guard`. The operator vendors the schema, selects that guard and reports cron runs in
its own pull request before a lake with this change is rolled.

## Methodology runs migration

`0078_methodology_runs` (lake#655) runs a workspace's own methodology, Python code in one of its
repositories, as untrusted code ([`../api/methodologies.md#runs`](../api/methodologies.md#runs)):

- `methodology_runs` (lake writes it and `NOTIFY`s `lake_services`): customer, workspace (routed by
  `enforce_workspace_routing`), `methodology_id` (`W-<8 hex>`, the entry in the workspace Zetta's
  registry), `mode` (`validate` or `train`), `build_id` (the methodology's image build), `image`
  (the built image's pull reference, empty until known), `runtime_image` (the pristine
  `oramasearch/methodology-runtime` image), `cpu_millis`, `memory_bytes`, `gpus`, `gpu_class`,
  `timeout_seconds`, `network` (false unless the person granted it), `slug` (`mr-<12 hex>`),
  `desired` (`present` or `absent`), `generation`, `stop_reason`;
- `methodology_run_status`, written only by the operator: `phase` (`Building`, `Staging`, `Running`,
  `Collecting`, `Succeeded`, `Failed`, `Stopped`; `Queued` from `0082`), `message`, `exit_code`, `credential_sha256` (the
  run's `lmr.` credential, never the credential), `observed_generation`, `started_at` and
  `finished_at` of the sandbox pod;
- `workload_resources.object_kind` adds `methodology_run` to every kind `0077_metered_jobs` allows.

Nothing about the run's content is kept here: its parameters, data, metrics, log and artifact live in
the workspace Zetta. Nothing is dropped; idempotent; guard `methodology_runs_guard`. The operator
vendors the schema, selects that guard and runs the pods in its own pull request
([orama-operator#291](https://github.com/oramasearch/orama-operator/issues/291)) before a lake with this
change is rolled.

## Store mounts migration

`0080_store_mounts` (lake#666) records which services and apps mount a store, and how much of a
store is used ([`storage.md#mounts`](storage.md#mounts)):

- `store_mounts` (lake writes it when a service or an app is created or edited with `stores`, in
  the transaction that bumps the workload's `generation` and `NOTIFY`s `lake_services`):
  `store_id` (cascades with the store), `object_kind` (`service` or `app`), `object_id`,
  `mount_path` (`''`, or an absolute path matching `^/[A-Za-z0-9._/-]{1,255}$`), `read_only`,
  `created_at`; primary key `(store_id, object_kind, object_id)`, index on
  `(object_kind, object_id)`;
- `store_status.used_bytes` and `used_bytes_at`, written only by the operator: the bytes in use
  and when they were measured (null until measured).

Content-free desired state: ids, a path and a flag. Nothing is dropped; idempotent; guard
`store_mounts_guard`. The operator vendors the schema, selects that guard, mounts the stores into
the workloads' pods and reports bytes in use in its own pull request before a lake with this change
is rolled.

## App transfer migration

`0081_app_transfer` (lake#661) adds `app_transfer`, written only by the Orama operator: one row per
app and UTC hour (`app_id`, `workspace_id`, `hour`, which must start on the hour, `in_bytes`,
`out_bytes`, `observed_at`), the bytes the app received and sent through the ingress controller in
that hour. The operator reads the controller's per-Ingress byte counters and adds each poll's
increase to the row, and deletes rows older than eight days. Numbers only: no host, path or content.
There is no foreign key to `apps`, so an hour stays chargeable after its app is deleted. Lake charges
each closed hour once in the app's workspace Zetta as `transfer` and records that it did there
(`lake_metering_app_transfer_records`, [`../api/metering.md#app-traffic`](../api/metering.md#app-traffic));
nothing about usage is kept here.

Nothing is dropped; idempotent; guard `app_transfer_guard`. Lake reads the table on every usage
sample, so the operator vendors the schema and selects that guard before a lake with this change is
rolled; on an older schema lake's sample names app traffic as unread and records the rest.

## Methodology run queue migration

`0082_methodology_run_queue` ([orama-operator#293](https://github.com/oramasearch/orama-operator/issues/293))
adds the phase `Queued` to `methodology_run_status.phase`. The operator writes it, with a message
saying why, while it holds a run back: its workspace already has
`spec.methodologies.maxConcurrentRunsPerWorkspace` runs staging, running or collecting (oldest run
first), or no node of a GPU run's GPU class exists to place its work volume next to. A queued run
has been reported on, so lake's 30-minute withdrawal of runs the operator never reported on does
not apply to it. Only the CHECK changes; nothing is dropped; idempotent; guard
`methodology_run_queue_guard`. The operator vendors the schema and selects that guard in the same
change.

## Methodology deployments migration

`0083_methodology_deployments` ([lake#690](https://github.com/oramasearch/lake/issues/690)) serves a
workspace methodology's model from Ship → Models. `methodology_deployments` holds one row per
deployment (lake writes it when the model is published and notifies `lake_services`): the workspace
methodology, the engine's model id, the training run and image build whose image serves it, the
pristine runtime image, CPU, memory, GPUs and GPU class, `network`, slug `md-<12 hex>`, `desired`
and `generation`; at most one present deployment per model of a workspace.
`methodology_deployment_status` is the operator's: phase (`Building`, `Queued`, `Staging`,
`Starting`, `Ready`, `Failed`, `Stopped`), message, the SHA-256 of the deployment's credential
(`lmd.<deployment id>.<secret>`), the serve Service's in-cluster `endpoint` (only
`http://md-<12 hex>.<namespace>.svc.cluster.local[:port]`), and when it became ready and stopped.
`workload_resources.object_kind` adds `methodology_deployment`, the serve pods' resources while they
run. Ids, images, resources and phases only: the model, its artifact and its predictions stay in the
workspace Zetta. Nothing is dropped; idempotent; guard `methodology_deployments_guard`. The operator
vendors the schema, selects that guard and runs the deployments in the same change.

## Repository workspaces migration

`0051_repository_workspaces` adds `repositories.workspace_id` (references `workspaces`, index
`repositories_workspace_idx`), the workspace each repository belongs to (lake#315, part 1). It is a
content-free routing pointer, like `stores.workspace_id` (`0044`): the repository's metadata stays
here and its content in the customer store until they move into that workspace's Zetta. Each
earlier repository, tombstoned ones included, is assigned its account's only live workspace
(`sole_workspace`); an account with several live workspaces or none is recorded as `needs_selection`
and reported by a warning. The `repository_workspace_routing` trigger runs
`enforce_workspace_routing`: a new repository must name a live workspace of its own account, and
neither `workspace_id` nor `customer_id` changes afterwards, so `transfer_repository` is refused and
an unassigned repository is assigned only by a reviewed migration. Nothing is dropped. Idempotent;
guard `repository_workspaces_guard`. The operator reads no repository table, but vendors the schema
and selects that guard before the binary is deployed. See
[`../api/repositories.md`](../api/repositories.md#the-owning-workspace).

## Unrouted repositories migration

`0066_assign_unrouted_repositories` (decision 54 S14, lake#513) is the reviewed assignment `0051`
left open. Once, each repository whose `workspace_id` is still NULL (tombstoned ones included) is
assigned its account's only live workspace when the account now has exactly one; a repository of a
account with several live workspaces or none is left alone and reported by a warning. Each assignment
is recorded in `workspace_assignments`: `repositories` from `needs_selection` to `sole_workspace`,
`repository_descriptions` (if `0064` recorded one) to `repository_workspace`. The trigger stays:
`enforce_workspace_routing` allows a change of `workspace_id` only on `repositories`, from NULL to a
live workspace of the same account, while the transaction-local setting `lake.repository_assignment`
is `0066_assign_unrouted_repositories`; the block sets it around its updates and clears it, and every
other change is refused as before. Nothing is added or dropped but the guard. Idempotent; guard
`assign_unrouted_repositories_guard`. The operator vendors the schema and selects that guard. See
[`../api/repositories.md`](../api/repositories.md#the-owning-workspace).

## Workspace store migration

`0067_workspace_store` (lake#380, decision 54 S1-S3) adds `workspaces.store_moved_at timestamptz`,
NULL until `move_workspace_store` switches the workspace: from then on its repositories are served
from its own encrypted liquid store in bucket `lake_store` of its workspace Zetta
([`../architecture/workspace-store.md`](../architecture/workspace-store.md)). The column is the only
thing PostgreSQL keeps about that store; the operator does not read it. Idempotent; guard
`workspace_store_guard`; the latest guard is `usage_limits_guard` (migration `0072`), which the operator selects.

## Metering workspaces migration

`0053_metering_workspaces` (lake#348, lake#349) retires `metering_usage` and
`metering_build_records`: lake no longer writes or reads them. Repository transfer and build
records live in the workspace Zetta of the repository's workspace
([`../api/metering.md`](../api/metering.md)), and repository storage is measured live on each read
and stored nowhere; with lake#380 a switched workspace's storage is its bucket's `usage`. The migration only records each row's
owner in `workspace_assignments` for the operator's copy: an account with exactly one live workspace
owns its rows (`sole_workspace`), every other row is `needs_selection` and reported by a warning.
A usage row's `object_id` is `md5(customer_id || E'\n' || hour)` with the hour printed as
`YYYY-MM-DDTHH:MI:SSZ` in UTC, a build row's `md5(customer_id || E'\n' || result_id)`, both as a
UUID, since neither table has an id column; lake computes the same. Usage rows without transfer
(only the retired gauge and compute) are not assigned. No column is added and nothing is dropped;
`metering_guard` stays. Idempotent; guard `metering_workspaces_guard`. The operator reads neither
table, but vendors the schema and selects that guard before the binary is deployed.

## Forge record workspaces migration

`0054_forge_record_workspaces` (lake#341) retires `forge_records`: lake no longer writes or reads
it. Each forge record lives in the workspace Zetta of its repository's workspace
([`../api/forge.md`](../api/forge.md#where-forge-records-live)). The migration only records each
row's owner in `workspace_assignments` for the operator's copy: its repository's workspace
(`repository_workspace`); `needs_selection` while the repository has no workspace (`0051` could not
assign it), which the copy's rerun of the assignment resolves once the repository is assigned; or
`workspace_deleted` when that workspace is gone, which does not hold the copy back. Each unresolved
row is reported by a warning. No column is added and nothing is dropped. Idempotent; guard
`forge_record_workspaces_guard`. The operator reads no forge table, but vendors the schema and
selects that guard before the binary is deployed.

## Application user workspaces migration

`0055_application_user_workspaces` (lake#353, lake#354) retires `auth_app_users` and
`auth_app_organizations`: lake no longer writes or reads them. An application's users and
organizations live in the workspace Zetta of the application's workspace
([`../api/apps.md`](../api/apps.md#where-users-and-organizations-live-lake353-lake354)), with a pool
user's email and an organization's TAAP identity and name as claims written `if_absent` there. The
migration only records each row's owner in `workspace_assignments` for the operator's copy: the
application's workspace (`application_workspace`); `needs_selection` while the application has no
workspace (`0049` could not assign it), which the copy's rerun of the assignment resolves once it is
assigned; or `workspace_deleted` when that workspace is gone, which does not hold the copy back. A
user row has no id column, so its `object_id` is md5(application id, newline, user id) as a UUID,
the id lake computes too. Each unresolved row is reported by a warning. No column is added and
nothing is dropped; the `users` rows and the pool's credentials and sessions stay. Session
resolution no longer joins `auth_app_users`: a session issued in an application's pool is checked
against the user's record in the workspace before it is returned, and suspension and deletion
revoke the pool's sessions. Idempotent; guard `application_user_workspaces_guard`. The operator
reads neither table, but vendors the schema and selects that guard before the binary is deployed.

## Application credential workspaces migration

`0056_application_credential_workspaces` (lake#352, lake#355) retires `auth_app_signing_keys` and
`auth_app_webhooks`: an application's signing keys (one key ring record per application) and its
webhooks live in the workspace Zetta of the application's workspace (`auth_applications.workspace_id`,
`0049`), with each seed and secret kept as the secrets envelope the table held, inside the sealed
record. The migration records each key ring's owner (`object_id` is the application) and each
webhook's in `workspace_assignments`: `application_workspace`, `needs_selection` while the
application has no workspace (warned, and resolved by the copy once it is assigned), or
`workspace_deleted` when that workspace is gone (warned; it does not hold the copy back). Lake no
longer writes or reads either table except through the operator-only `copy_workspace_records`,
which reapplies the assignment before each run. The `keystore` rows of the `application.jwt.*` and
`application.webhook.*` purposes stay: they are the wrapped data keys that open the envelopes.
Nothing is dropped. Idempotent; guard `application_credential_workspaces_guard`. Mechanism and
cutover:
[`../architecture/workspace-records.md`](../architecture/workspace-records.md#cutover-runbook-application-keys-and-webhooks-dev-cluster).

## Application settings workspaces migration

`0059_application_settings_workspaces` (lake#351) moves an application's settings, its `name`,
`allowed_origins`, `redirect_uris` and `theme`, into one sealed record of the workspace Zetta of the
application's workspace (`auth_applications.workspace_id`, `0049`). `auth_applications` stays as the
content-free routing row: `id`, `customer_id` (the account and the policy object's parent), `pool_id`
(named by the pool's sessions, credentials and tokens), `custom_sign_in_domain` (the `Host` a
custom-domain page is requested on, unique across lake), `workspace_id`, `created_at` and
`updated_at`. The four settings columns lose NOT NULL and their defaults; lake writes NULL there for
every new application and reads them only through the operator-only `copy_workspace_records`
(collection `auth_applications`), and `auth_applications_settings_check` keeps a row without a name
free of settings. The migration records the owner of each row that still has a name in
`workspace_assignments` under `object_table = 'auth_application_settings'` (`0049` already records
the applications' routing under `auth_applications`): `application_workspace`, `needs_selection`
while the application has no workspace (warned, and resolved by the copy once it is assigned), or
`workspace_deleted` (warned; it does not hold the copy back). Nothing is dropped. Idempotent; guard
`application_settings_workspaces_guard`. Mechanism and cutover:
[`../architecture/workspace-records.md`](../architecture/workspace-records.md#cutover-runbook-application-settings-dev-cluster).

## Application authorization code workspaces migration

`0061_application_authorization_code_workspaces` (lake#363) moves an application's one-use OIDC
authorization codes into the workspace Zetta of the application's workspace
(`auth_applications.workspace_id`, `0049`): issued `if_absent`, redeemed by a delete at the revision
read, expired by the collection's five-minute retention. Lake no longer writes or reads
`auth_app_authorization_codes` except through the operator-only `copy_workspace_records`
(collection `auth_app_authorization_codes`), which considers only live codes (unused, unexpired)
and reapplies the assignment before each run. The migration records each live code's owner in
`workspace_assignments` under `object_table = 'auth_app_authorization_codes'` and
`object_id = md5(code_hash)::uuid`: `application_workspace`, `needs_selection` while the application
has no workspace (warned, and resolved by the copy once it is assigned), or `workspace_deleted`
(warned; it does not hold the copy back). The copy sets a code's outcome to `copied` before it
writes it, and never writes a `copied` code again. Used and expired codes are not recorded. Nothing
is dropped. Idempotent; guard `application_authorization_code_workspaces_guard`. Mechanism and
cutover:
[`../architecture/workspace-records.md`](../architecture/workspace-records.md#cutover-runbook-application-authorization-codes-dev-cluster).

## Agent task workspaces migration

`0062_agent_task_workspaces` (lake#374, lake#375; [`0070`](#drop-moved-tables-migration) drops the three content columns) moves what was written for an agent task, its
request `body`, the agent's `message` and its `result`, into one sealed record of the workspace
Zetta of the task's workspace (`agent_tasks.workspace_id`, `0038`). `agent_tasks` and
`agent_task_status` stay as the content-free dispatch rows the agent bus, the claim and its lease
(`0042`) use without a workspace call. It adds `agent_tasks.parent_task_id` (uuid, the asking task
of a delegated turn, taken from the body of existing rows) and `agent_task_status.notice` (text,
default `''`: lake's own retry and failure text, taken from existing rows whose message is one, and
never what a person or an agent wrote). Lake writes `body = '{}'`, `message = ''` and
`result = null` on every new row and reads those three columns only through the operator-only
`copy_workspace_records` (collection `agent_tasks`). The migration records the owner of each task
that still holds content in `workspace_assignments` under `object_table = 'agent_task_content'`
(`0038` already records the tasks' routing under `agent_tasks`): `task_workspace`,
`needs_selection` while the task names no workspace (warned), or `workspace_deleted` (warned; it
does not hold the copy back). The operator reads neither table. Nothing is dropped. Idempotent;
guard `agent_task_workspaces_guard`. Mechanism and cutover:
[`../architecture/workspace-records.md`](../architecture/workspace-records.md#cutover-runbook-agent-tasks-dev-cluster).

## MCP connection dispatch migration

`0079_mcp_connection_dispatch` (lake#684, lake#685) records where a workspace's MCP connections need
Lake outside a request ([`../api/mcp-connections.md`](../api/mcp-connections.md)):

- `mcp_connection_dispatch` (lake writes it with every connection it writes): `customer_id`,
  `workspace_id` (both cascading on delete), `connection_id` (the connection record's id in the
  workspace Zetta), `streams` (it has a notifications topic, an agent with an input topic, or results
  still to publish), `exported_digest` (the hex SHA-256 of the `tools/list` entries it re-exports
  through Lake's MCP, or empty), `updated_at`. A connection with neither has no row. The partial index
  `mcp_connection_dispatch_streams_idx` serves the stream worker's read.

The connection itself, its tools and its settings stay in the workspace Zetta. Nothing is dropped;
idempotent; guard `mcp_connection_dispatch_guard`. The operator reads nothing of it; it vendors the
schema and selects that guard before a lake with this change is rolled.

## Cloud orders migration

`0089_cloud_orders` (lake#831, [`../api/orders.md`](../api/orders.md)) records the apex order
sheet's orders. It is the contract the Orama operator provisions from
(https://github.com/oramasearch/orama-operator/issues/634); the names below are fixed.

`cloud_orders`, one row per order, kept (a billing record):

| Column | Type | Written by | Meaning |
|---|---|---|---|
| `id` | `uuid` PK, `gen_random_uuid()` | lake | The order id people and emails quote |
| `created_at`, `updated_at` | `timestamptz` | lake; `updated_at` by the trigger `cloud_orders_touch` on every update, the operator's included | |
| `offer` | `text`, `private_warehouse`, `dedicated_cluster`, `on_prem` or `shared` | lake | |
| `path` | `text`, `checkout` or `order_form` | lake | `checkout` only for `private_warehouse` and `dedicated_cluster` (`cloud_orders_checkout_offer`), and only with `quantity` 1 (`cloud_orders_checkout_one`) |
| `cloud_provider` | `text NOT NULL DEFAULT ''`, ≤ 64 | lake | The configuration's provider id (`azure`, `aws`, `gcp`); empty for a private warehouse (shared hardware), on-prem and anything not listed |
| `region` | `text NOT NULL DEFAULT ''`, ≤ 64 | lake | The cloud region (`canadacentral`), or a shared region's name for a private warehouse; empty when not listed |
| `contact_name` (1–200), `contact_email` (3–254), `company` (≤ 200, default `''`), `details` (≤ 4,000, default `''`) | `text` | lake | What the person typed; `details` holds anything not listed (other providers, hardware, regions, multi-region, quantities) |
| `quantity` | `integer`, 1–1,000, default 1 | lake | Always 1 on a `checkout` order (`cloud_orders_checkout_one`): a paid order is one warehouse or one cluster with one `host`; more is an order form |
| `amount_cents` | `bigint` ≥ 0 | lake | The monthly fee for `quantity` |
| `currency` | `text`, default `'usd'` | lake | |
| `included_credit_cents` | `bigint` ≥ 0, default 0 | lake | The usage credit included each month |
| `stripe_checkout_session_id` | `text` UNIQUE, nullable | lake | The Checkout Session (`cs_…`) |
| `stripe_customer_id`, `stripe_subscription_id` | `text`, nullable | lake, from Stripe's webhook | |
| `state` | `text`: `submitted`, `pending_payment`, `paid`, `provisioning`, `ready`, `failed`, `cancelled` | lake and Stripe up to `paid` or `cancelled`; **the operator** from `paid` on | |
| `host` | `text`, nullable, ≤ 253 | **the operator**, with `ready` | Where the provisioned Lake answers |
| `failure_reason` | `text`, nullable, ≤ 2,000 | **the operator**, with `failed` | Emailed to support and the customer |
| `claimed_by` | `text`, nullable, ≤ 255 | **the operator** | Which operator took the order |
| `claimed_at` | `timestamptz`, nullable | **the operator** | |
| `organization_id` | `uuid`, nullable, no foreign key | lake (lake#832) | The organization the order belongs to when the person ordering is signed in and chooses one; null for anonymous orders, which is every order today |
| `support_emailed_state`, `customer_emailed_state` | `text`, nullable | lake | The last state each recipient was emailed about |
| `email_attempts` | `integer` ≥ 0, default 0 | lake | Failed attempts since the last delivery |
| `email_error` | `text NOT NULL DEFAULT ''`, ≤ 1,024 | lake | The last failure, or `LAKE_RESEND_API_KEY is not set` |
| `email_retry_at` | `timestamptz`, nullable | lake | When the order pass may try again (also its five-minute hold) |

`cloud_order_events`, the history, one row per state change:

| Column | Type | Meaning |
|---|---|---|
| `id` | `bigserial` PK | Order of events |
| `order_id` | `uuid` → `cloud_orders(id)` ON DELETE CASCADE | |
| `at` | `timestamptz`, default `now()` | |
| `from_state` | `text`, nullable (null when the order is created) | One of the states above |
| `to_state` | `text` | One of the states above |
| `actor` | `text`: `lake`, `stripe` or `operator` | Who changed it |
| `detail` | `text NOT NULL DEFAULT ''`, ≤ 2,000 | The Stripe event, the reason, the operator's note |
| `audited_at` | `timestamptz`, nullable | Set by lake once the event is in the central audit log; **the operator leaves it null** |

**The operator's part**: read `paid` rows (index `cloud_orders_paid_idx`, or listen on
`lake_cloud_orders`, which the trigger `cloud_orders_paid` notifies with the order id when an order
becomes `paid`); claim one with `UPDATE … SET state = 'provisioning', claimed_by = $me, claimed_at =
now() WHERE id = $1 AND state = 'paid' AND claimed_by IS NULL`; then write `ready` with `host` or
`failed` with `failure_reason`; and insert a `cloud_order_events` row (`actor = 'operator'`) for
each change. It writes no other column. Lake emails the customer on `ready` and `failed`, and
support on `failed`, and records every event in the central audit log.

Indexes: `cloud_orders_paid_idx` (`created_at` where `state = 'paid'`), `cloud_orders_email_due_idx`,
`cloud_order_events_order_idx` (`order_id, id`) and `cloud_order_events_unaudited_idx`. Additive and
idempotent; guard `cloud_orders_guard`. The operator vendors this schema and selects that guard
before a lake with this change is rolled.

