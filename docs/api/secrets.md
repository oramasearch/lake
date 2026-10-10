# Static secrets

**What.** A secret mount belongs to one organization inside one customer namespace, and lives in
one workspace of that space (lake#328). Each name in the mount is a versioned JSON object. Lake
keeps immutable version history, custom metadata and the current-version pointer, all in the
mount's workspace Zetta. The API is lake-native; it does not reproduce another service's URL
layout.

## Capabilities

| Capability | Authorization | Input | Result |
|---|---|---|---|
| `create_secret_mount` | `admin` on the organization | `customer_id`, `workspace_id`, `organization_id`, lowercase `name`, `description?` | mount metadata, with its `workspace_id` |
| `list_secret_mounts` | candidate-filtered `read` on each mount | `customer_id`, `workspace_id`, `organization_id?`, `limit?` | readable mounts of that workspace only |
| `get_secret_mount` | `read` on the mount | `mount_id` | mount metadata |
| `put_secret` | `write` on the mount | `mount_id`, `name`, object `value`, object `custom_metadata?`, `check_and_set?` | the new version and supplied value |
| `read_secret` | `read` on the mount | `mount_id`, `name`, `version?` | the selected live version and value |
| `secret_metadata` | `read` on the mount | `mount_id`, `name` | current version, custom metadata and every version's state |
| `list_secrets` | `read` on the mount | `mount_id`, `prefix?`, `cursor?`, `limit?` (1-500, default 100) | `items` (path and metadata, never a value), `next_cursor` when more follow |
| `delete_secret_versions` | `write` on the mount | `mount_id`, `name`, positive `versions[]` | updated metadata |
| `undelete_secret_versions` | `write` on the mount | same | updated metadata |
| `destroy_secret_versions` | `write` on the mount | same | updated metadata |

`version` omitted or zero reads the current version. A nonzero `check_and_set` must equal the
current version or the put returns `conflict`; zero makes no concurrency assertion. Creating the
first value starts at version 1. Supplying nonempty custom metadata replaces the entry metadata;
omitting it preserves existing metadata.

`list_secrets` (lake#561) answers a mount's secrets in name order: each item's `name`,
`current_version`, `created_at`, `updated_at`, and the current version's state, `deleted`
(true once it is deleted or destroyed) with `deleted_at` and `destroyed_at` (null while live). It
never returns a value, never decrypts and never reads the data key. `prefix` keeps the names that
start with it. When more names follow, `next_cursor` is the last name answered; pass it back as
`cursor` for the next page, which starts after it. It is read-only (`readOnlyHint` in MCP),
authorizes the one mount itself before the workspace is reached (like every `list_*`), and its
event goes to the mount's workspace audit, naming the mount and the prefix.

Delete is reversible: it sets `deleted_at`, makes that version unreadable and retains ciphertext.
Undelete clears `deleted_at` unless the version was destroyed. Destroy clears the ciphertext and
sets both deletion and destruction timestamps. Its history row remains, but reads and undelete
return an error. A later put creates the next version and never reuses a number.

## Where secrets live (lake#328-#332)

`create_secret_mount` requires `workspace_id`: a live workspace of `customer_id`'s space. It is
recorded once and never changes. There is no default workspace and no fallback: an omitted,
malformed or unknown `workspace_id` is `invalid_input` at `/workspace_id`, and a workspace of
another space is `invalid_input` at `/customer_id`. A space may hold mounts in several workspaces,
and a mount name is unique within its workspace and organization (`conflict` at `/name`).

Every other capability names the mount, and the mount names its workspace, so none of them takes
`workspace_id`: lake reads the mount's route and works only in that workspace, through the
workspace caller, which checks the workspace's space and readiness on every call. A workspace that
is not ready answers `workspace_not_ready`; a deleted workspace took its mounts with it.

| Where | What |
|---|---|
| Workspace Zetta, `lake_secrets_mounts` | The mount (name, description, organization), keyed by its id, and a name claim keyed by the blind index of organization and name, written `if_absent` |
| `lake_secrets_entries` | One record per secret name: current version and custom metadata. `list_secrets` reads the mount's entries by their lookup, then each one's current version record for its state |
| `lake_secrets_versions` | One record per version: the existing AES-256-GCM ciphertext, soft-delete and destroy instants |
| `lake_secrets_encryption_keys` | One record per named key: its id (which every transit ciphertext names), current version, derivation parent |
| `lake_secrets_data_keys` | One record per data key version: the mount's data key and each named-key version, wrapped by the customer KEK, with the KEK version that wraps it |
| PostgreSQL `secret_mount_routes` | Mount id → space, organization, workspace. No name, value or key: dispatch only |
| PostgreSQL `objects` | The mount's policy object, under `organization → environment` |
| PostgreSQL `keystore` | The customer KEK, wrapped by `LAKE_INSTANCE_KEK` (lake#324 governs its own move) |

Every record is sealed with the workspace's record key and found by blind indexes
([workspace-records.md](../architecture/workspace-records.md)); names select records only through
HMACs, so the workspace holds no secret, mount or key name in the clear. Inside the seal the
envelope is the one lake has always used: `LAKE_INSTANCE_KEK` wraps the customer KEK, the customer
KEK wraps each data key (associated data `lake data key/<customer>/<owner>/<version>`), and the
mount data key seals each version (associated data binds mount, name and version). A copied
ciphertext is byte-for-byte the one PostgreSQL held.

**Version transitions are conditional writes.** A put claims the next version number with
`if_absent`, then moves the entry's current version forward at the revision it read; two writers
never get the same number, and a `check_and_set` that no longer holds is `conflict` at
`/check_and_set`. If a put stopped after claiming its number but before moving the entry, the next
writer finds the number taken, moves the entry to the newest version that exists (as the stopped
put would have) and continues; a `check_and_set` writer gets `conflict` and rereads. Delete,
undelete and destroy each rewrite one version record at the revision read and reread on a
conflict, so a destroy is never undone by a write computed from an older copy. Destroy replaces
the record without its ciphertext; the history record remains. A later put never reuses a number.

**Customer KEK rotation** (`rotate_customer_kek`) makes a new KEK current and keeps the replaced
one until every data key in every workspace of the space that holds a mount has been rewrapped at
the revision read; then it forgets the old one. A workspace that is not ready stops the rotation
with `workspace_not_ready`: data keys there stay readable under the kept KEK, and the next
rotation finishes the earlier one before it starts. A data key written while a rotation runs is
rewrapped by its writer if the KEK changed underneath it. PKI authorities (still in PostgreSQL) are
rewrapped in the same transaction as the KEK change.

**Authorization** is unchanged: mount creation adds `organization → environment → secret_mount` to
the policy graph, and mount grants are inherited through the normal policy engine. A policy
decision reads only PostgreSQL, so a refused caller causes no workspace call; `list_secret_mounts`
authorizes the workspace's routes with `authorize_batch` before it reads the workspace, and omits
mounts the caller cannot read. Per-name `secret` policy objects are no longer created (they carried
names); the ones 0009 made stay until the tables are dropped.

Every call passes through `Registry::call` and `audit::record`. Secret audit events identify the
customer, caller, mount, workspace, name and version or version set; values and ciphertext are
never audit details. A policy refusal records outcome `denied` against `secret_mount/<mount-id>`.

## Migration and cutover (lake#328-#332)

Migration `0057_secret_workspaces` adds `secret_mount_routes`, gives each mount of a space with
exactly one live workspace that workspace (`sole_workspace`) and its route, and reports every other
mount as `needs_selection`. A mount's entries, versions, keys and data keys go where it goes and
have no assignment rows of their own. It also keeps the previous customer KEK in `keystore`
(`previous_wrapped_key`, `previous_version`) and drops `secrets_dynamic_leases`' foreign key to
`secrets_mounts`. Nothing is dropped. The five 0009 tables were read only by the one-shot copy,
`copy_workspace_records` with collection `secrets_mounts`, `secrets_data_keys`,
`secrets_encryption_keys`, `secrets_entries` or `secrets_versions`; verify compared data keys by
unwrapping both sides, so a KEK rotation after the copy was not a difference. The copy was
verified on dev, and migration `0070_drop_moved_tables` drops the five tables (lake#311); the
collections are no longer accepted, and a customer KEK rotation rewraps only the workspace data
keys and the PKI authorities. `secret_mount_routes` and the `secret` policy objects stay. The dev cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-secrets-dev-cluster).

The operator does not read secrets: `LakeSealedSecret` with a `lake` source asks lake over HTTP
(`mint_sealed_secret`), never PostgreSQL, and that route names a mount, which names its workspace.

## Dynamic leases

| Capability | Authorization | Input | Result |
|---|---|---|---|
| `lease_dynamic_secret` | `write` on the mount | `mount_id`, `role`, `lifetime_seconds?` (1-3600, default 300) | `lease` metadata and the generated `value`, once |
| `renew_dynamic_secret` | `write` on the lease's mount | `lease_id`, `lifetime_seconds?` | lease metadata |
| `revoke_dynamic_secret` | `write` on the lease's mount | `lease_id` | lease metadata |

`lease_dynamic_secret` generates a response-only username and password for a named role. Lake seals
them with the mount data key read from the mount's workspace (associated data
`lake dynamic secret/<mount>/<lease id>`) and keeps the lease in that workspace. The response
includes the lease metadata (`id`, `mount_id`, `workspace_id`, `role`, `expires_at`, `revoked_at`,
`expired_at`, `renewals`, `created_at`) and the value once. `renew_dynamic_secret` extends an
active, unexpired lease to `now + lifetime_seconds` (never shortening it) and increments its renewal
count; a revoked or expired lease is `conflict`. `revoke_dynamic_secret` records immediate
revocation, erases the sealed value and is idempotent. Lifetimes are positive and capped at one
hour.

### Where leases live (lake#362)

| Where | What |
|---|---|
| Workspace Zetta, `lake_secrets_dynamic_leases` | One record per lease, keyed by the lease id, in its mount's workspace: mount, role, the sealed value (null once revoked or expired), expiry, revocation and expiry instants, renewals |
| PostgreSQL `secret_lease_routes` | Lease id → mount, space, workspace, and `due_at`: when the expiry loop must next look at the lease. No role, value or ciphertext: dispatch only |

A lease belongs to its mount's workspace; neither changes. Renew and revoke name only the lease, so
lake reads the lease's route to find the mount (whose route decides authorization, from PostgreSQL
only: a refused caller reaches no workspace) and works only in that workspace. A malformed or
unknown `lease_id` is `not_found` at `/lease_id`; a missing one is `invalid_input`. A workspace that
is not ready answers `workspace_not_ready`, and a deleted workspace took its leases with it.

**Every change is a conditional write.** A new lease's route is written first, then its record
`if_absent`. Renew, revoke and the expiry loop each rewrite the record at the engine revision they
read and reread on a conflict. A renewal that read the lease active therefore cannot land on a lease
another process revoked or expired in the meantime: its write fails, it rereads, finds the lease
settled and answers `conflict`. A revocation or expiry is never undone.

**Expiry runs on a schedule, from a due index.** The server's owner loop (every
`secret_lease_expiry_interval`, ten seconds by default) runs one expiry pass. It does not read every
workspace. It claims the routes whose `due_at` has passed from `secret_lease_routes`, earliest first,
with `FOR UPDATE SKIP LOCKED`, and holds each for 60 seconds so another replica's pass does not take
it. Then, in each claimed lease's workspace only:

- an expired lease gets `expired_at` and its sealed value erased;
- a revoked lease whose value is still there has it erased;
- a lease renewed since it was indexed gets its new expiry as `due_at`.

A settled lease's `due_at` becomes NULL and is never set again. `due_at` is a hint that is never
later than the expiry in the record. A renewal only moves the expiry forward, and the record, not the
index, decides whether a lease is active. So a lost or reordered index write only makes the pass look
at a lease early. A workspace that is not ready keeps its leases claimed until the hold passes, and
they are tried again then. A deleted workspace's routes are settled without a call. A route whose
record was never written is removed after five minutes: a create stopped between its two writes,
and its caller got an error. Counters: `lake_secret_leases_settled_total{outcome}` and
`lake_secret_lease_sweep_errors_total`.

Each lifecycle call records a signed `dynamic_secret` audit event whose object is the lease ID and
whose details name only the mount, workspace, lease, operation, and outcome. When a transparency log
is configured, that signed audit record is also appended as a digest-checked `audit_event/1` entry.
No generated username, password, ciphertext, or KEK is present in the audit event or log entry. The
expiry pass is not a capability call and records no audit event.

### Migration and cutover (lake#362)

Migration `0058_secret_lease_workspaces` adds `secret_lease_routes` (immutable workspace through
`enforce_workspace_routing`, a foreign key to the mount's route, a partial index on `due_at`) and
reports each legacy lease whose mount has no route. A lease follows its mount: it has no assignment
rows of its own. Nothing is dropped. `copy_workspace_records` with collection
`secrets_dynamic_leases` wrote each lease's route (due at its expiry, or at once if revoked) and
then its record, copying the ciphertext unopened; verify accepted later renewals, revocations and
expiries. The copy was verified on dev, and migration `0070_drop_moved_tables` drops
`secrets_dynamic_leases`; `secret_lease_routes` stays. The dev cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-dynamic-leases-dev-cluster).

## What the exit scenario proves

The identity-exit Postgres scenario leases a generated credential, renews it, revokes it, reads the
three signed audit rows by lease object, observes three transparency leaves, and verifies the
revocation leaf's compact inclusion proof with libliquid's verifier.

## Page and verification

The web app manages secrets through these capabilities ([../ui/secrets.md](../ui/secrets.md)):
`/secret-mounts?workspace_id=<uuid>` lists the readable mounts of one workspace and creates one;
`/secrets?mount_id=<uuid>` lists a mount's secrets (`list_secrets`) and writes a new one;
`/secrets?mount_id=<uuid>&name=<name>` shows a secret's versions, writes the next one with
check-and-set, reveals one version's value on a deliberate click (`read_secret`, held only in the
page's memory) and deletes, undeletes or destroys versions; `/encryption-keys?mount_id=<uuid>`
creates, derives and rotates keys and tries encrypt, decrypt, rewrap, sign, verify and HMAC. A value
never appears in a URL, a log or browser storage.

| Test | Covers |
|---|---|
| `SecretsCapabilities.ListSecretsAnswersNamesAndStateNeverValues` | `list_secrets`: name order, current version, times and deleted/destroyed state, no value in the answer, `prefix`, the cursor walking every name once, `limit` bounds, an unknown mount, read-only and candidate-authorizing, classified as workspace audit, an ungranted caller refused and audited before any workspace call |
| `SecretsCapabilities.*` | The capability contract on workspace records: history, delete/undelete/destroy, number reuse, a stopped put completed by the next, transit, KEK rotation finished after a workspace was not ready, dynamic leases, denial and audit, an ungranted list reaching no workspace |
| `Stores/WorkspaceRecordStores.SecretsLiveInTheirMountsWorkspace/{Memory,Postgres}` | Two spaces with two workspaces each, records sealed only at their own endpoint and absent from PostgreSQL, duplicate names, missing, malformed, unknown and foreign `workspace_id`, unknown `mount_id`, another space's person, a record copied elsewhere not opening, a workspace not ready, KEK rotation across workspaces, workspace deletion |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesSecretsIntoTheirMountsWorkspace/{Memory,Postgres}` | The five collections: plan, copy, rerun, verify, legacy ciphertexts read and decrypted through the capabilities, a later put, undelete and KEK rotation accepted, `needs_selection` resolved, `workspace_deleted` not blocking, tamper detection, operator only |
| `PostgresSecrets.MigrationRecordsEachMountsOwnerAndRoute` | Migration 0057 on a rerun: outcomes, routes, the dropped foreign key, immutable routes, a reviewed assignment, late mounts, every child row following its mount |
| `PostgresCapabilities.SecretsAndEncryptionRoundTrip`, `IdentityExit.*` | The round trip against the numbered schema with nothing but the route in PostgreSQL; leases on a workspace mount |
| `SecretsCapabilities.DynamicLeasesLiveInTheMountsWorkspace`, `…ARenewalRacingARevocationOrAnExpiryCannotReviveTheLease`, `…TheExpiryLoopReadsOnlyTheWorkspacesOfDueLeases`, `…ARenewalThatBeatsTheExpiryLoopIsKept`, `…TheServersOwnerLoopSettlesDueLeases` | Leases (lake#362): a sealed record and a content-free route, renewal moving the due index, revocation erasing the value and settling the route, malformed and unknown `lease_id`; a renewal racing a revocation or an expiry refused, an expiry pass racing a renewal keeping it; the pass reading no workspace without a due lease, holding claims, a not-ready and a deleted workspace, a route without a record kept while fresh; the server tick |
| `Stores/WorkspaceRecordStores.DynamicLeasesLiveInTheirMountsWorkspace/{Memory,Postgres}`, `…TheOneShotCopyMovesDynamicLeasesIntoTheirMountsWorkspace/{Memory,Postgres}`, `PostgresSecrets.LeaseRoutesAreAContentFreeDueIndex` | Two spaces with two workspaces each, records only at their own endpoint and none in PostgreSQL, another space's person refused before any workspace call, a record copied elsewhere, not ready, expiry in its own workspace, an abandoned route removed, workspace deletion; the copy (plan, copy, rerun, verify, renewal and expiry after the copy, `needs_selection` resolved, tamper detection, operator only); migration 0058, the claim and hold, settled routes never due again, the legacy listing |

The pure page renderer test checks escaping and absence of secret values.
