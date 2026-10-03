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
| `delete_secret_versions` | `write` on the mount | `mount_id`, `name`, positive `versions[]` | updated metadata |
| `undelete_secret_versions` | `write` on the mount | same | updated metadata |
| `destroy_secret_versions` | `write` on the mount | same | updated metadata |

`version` omitted or zero reads the current version. A nonzero `check_and_set` must equal the
current version or the put returns `conflict`; zero makes no concurrency assertion. Creating the
first value starts at version 1. Supplying nonempty custom metadata replaces the entry metadata;
omitting it preserves existing metadata.

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
| `lake_secrets_entries` | One record per secret name: current version and custom metadata |
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
`secrets_mounts`. Nothing is dropped. The five 0009 tables are read only by the one-shot copy,
`copy_workspace_records` with collection `secrets_mounts`, `secrets_data_keys`,
`secrets_encryption_keys`, `secrets_entries` or `secrets_versions`; verify compares data keys by
unwrapping both sides, so a KEK rotation after the copy is not a difference. The dev cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-secrets-dev-cluster).

The operator does not read secrets: `LakeSealedSecret` with a `lake` source asks lake over HTTP
(`mint_sealed_secret`), never PostgreSQL, and that route names a mount, which names its workspace.

## Dynamic leases

`lease_dynamic_secret` generates a response-only username and password for a named role and stores
their encrypted JSON under the mount data-key hierarchy: lake seals it with the mount data key read
from the mount's workspace, and the lease row itself stays in PostgreSQL until lake#362 moves it. The response includes lease metadata and
the value once. `renew_dynamic_secret` extends an active, unexpired lease and increments its
renewal count. `revoke_dynamic_secret` records immediate revocation and is idempotent. Initial and
renewed lifetimes are positive and capped at one hour.

Each lifecycle call records a signed `dynamic_secret` audit event whose object is the lease ID and
whose details name only the mount, lease, operation, and outcome. When a transparency log is
configured, that signed audit record is also appended as a digest-checked `audit_event/1` entry.
No generated username, password, ciphertext, or KEK is present in the audit event or log entry.

## What the exit scenario proves

The identity-exit Postgres scenario leases a generated credential, renews it, revokes it, reads the
three signed audit rows by lease object, observes three transparency leaves, and verifies the
revocation leaf's compact inclusion proof with libliquid's verifier.

## Page and verification

`GET /secret-mounts?workspace_id=<uuid>` lists the readable mounts of one workspace (without the
parameter, the workspace the viewer last chose, or a prompt to choose one).
`GET /secrets?mount_id=<uuid>&name=<name>` shows custom metadata and version states but
deliberately does not render values. Mutations remain capability calls.

| Test | Covers |
|---|---|
| `SecretsCapabilities.*` | The capability contract on workspace records: history, delete/undelete/destroy, number reuse, a stopped put completed by the next, transit, KEK rotation finished after a workspace was not ready, dynamic leases, denial and audit, an ungranted list reaching no workspace |
| `Stores/WorkspaceRecordStores.SecretsLiveInTheirMountsWorkspace/{Memory,Postgres}` | Two spaces with two workspaces each, records sealed only at their own endpoint and absent from PostgreSQL, duplicate names, missing, malformed, unknown and foreign `workspace_id`, unknown `mount_id`, another space's person, a record copied elsewhere not opening, a workspace not ready, KEK rotation across workspaces, workspace deletion |
| `Stores/WorkspaceRecordStores.TheOneShotCopyMovesSecretsIntoTheirMountsWorkspace/{Memory,Postgres}` | The five collections: plan, copy, rerun, verify, legacy ciphertexts read and decrypted through the capabilities, a later put, undelete and KEK rotation accepted, `needs_selection` resolved, `workspace_deleted` not blocking, tamper detection, operator only |
| `PostgresSecrets.MigrationRecordsEachMountsOwnerAndRoute` | Migration 0057 on a rerun: outcomes, routes, the dropped foreign key, immutable routes, a reviewed assignment, late mounts, every child row following its mount |
| `PostgresCapabilities.SecretsAndEncryptionRoundTrip`, `IdentityExit.*` | The round trip against the numbered schema with nothing but the route in PostgreSQL; leases on a workspace mount |

The pure page renderer test checks escaping and absence of secret values.
