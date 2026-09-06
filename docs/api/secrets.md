# Static secrets

**What.** A secret mount belongs to one organization inside one customer namespace. Each name in
the mount is a versioned JSON object. Lake keeps immutable version history, custom metadata and
the current-version pointer. The API is lake-native; it does not reproduce another service's URL
layout.

## Capabilities

| Capability | Authorization | Input | Result |
|---|---|---|---|
| `create_secret_mount` | `admin` on the organization | `customer_id`, `organization_id`, lowercase `name`, `description?` | mount metadata |
| `list_secret_mounts` | candidate-filtered `read` on each mount | `customer_id?`, `organization_id?`, `limit?` | readable mounts only |
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

## Storage and authorization

`secrets_mounts`, `secrets_entries` and `secrets_versions` are migration `0009_secrets`. Secret
values are serialized as JSON and encrypted with AES-256-GCM under the mount data key. Associated
data binds the ciphertext to mount, name and version. Only custom metadata and lifecycle
timestamps are plaintext database fields.

Mount creation adds `organization → environment → secret_mount` to the policy graph. The
environment has stable external id `secrets/<customer-id>/<organization-id>`. The first put of a
name adds a child `secret` object. Mount grants are therefore inherited through the normal policy
engine. A list calls `authorize_batch` and omits mounts the caller cannot read.

Every call passes through `Registry::call` and `audit::record`. Secret audit events identify the
customer, caller, mount, name and version or version set; values and ciphertext are never audit
details. A policy refusal records outcome `denied` against `secret_mount/<mount-id>`.

## Dynamic leases

`lease_dynamic_secret` generates a response-only username and password for a named role and stores
their encrypted JSON under the mount data-key hierarchy. The response includes lease metadata and
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

`GET /secret-mounts` lists readable mounts. `GET /secrets?mount_id=<uuid>&name=<name>` shows custom
metadata and version states but deliberately does not render values. Mutations remain capability
calls.

`SecretsCapabilities.VersionHistoryDeleteUndeleteAndDestroy` exercises the in-memory contract.
`PostgresCapabilities.SecretsAndEncryptionRoundTrip` exercises the same capability calls against
the numbered schema. The pure page renderer test checks escaping and absence of secret values.
