# Secrets and Encryption Keys

Lake keeps an organization's secrets (passwords, API keys, connection strings) and its encryption
keys. Secrets are versioned JSON values you write and read through the API. Encryption keys never
leave lake: you send data to lake and get back ciphertext, a signature or an HMAC.

The web app shows only **metadata**: which mounts exist, a secret's version history, and a key's
versions. Values and key material are never shown on a page.

## Concepts

| Term | What it is |
|---|---|
| **Secret mount** | A named container of secrets and keys that belongs to one organization and lives in one workspace, chosen when it is made and never changed. Access is granted on the mount. |
| **Secret** | A name in a mount (such as `database/password`) holding a JSON object, with an immutable history of versions. |
| **Version** | One value of a secret. Writing again creates the next version; numbers are never reused. |
| **Delete, undelete, destroy** | Delete hides a version but keeps it (reversible); undelete brings it back; destroy erases its data for good, keeping only its history row. |
| **Encryption key** | A named, versioned key in a mount, used for encrypt and decrypt, sign and verify, HMAC and derivation. Rotation adds a version; old versions still decrypt and verify. |
| **Dynamic secret lease** | A generated username and password for a role, returned once and valid for a limited time, renewable and revocable. |
| **Account key (KEK)** | The key that wraps every mount's data keys for one account. It can be rotated. |

Everything about a mount (its secrets, versions and keys) is sealed in that workspace's Zetta.
Deleting the workspace deletes its mounts.

## Find Your Secret Mounts

1. Open **Organization → Secrets**. The **Secret Mounts** page lists the mounts you can read in one
   workspace.
2. If you have more than one workspace, choose it in **Workspace**.
3. Each mount has **Secrets** and **Keys** buttons.

- REST: `GET /v1/list_secret_mounts?customer_id=<customer id>&workspace_id=<workspace id>&limit=100`.
- MCP: `list_secret_mounts` with the same input.

## Create a Mount

An organization admin creates mounts; the page has no form for it.

- REST: `POST /v1/create_secret_mount` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "organization_id": "<organization id>", "name": "payments", "description": "Payment service credentials"}`.
- MCP: `create_secret_mount`.

The name is lowercase and unique within its workspace and organization. The answer's `id` is the
`mount_id` every other call takes; none of them takes `workspace_id`, because the mount names its
workspace.

## Write a Secret

- REST: `POST /v1/put_secret` with
  `{"mount_id": "<mount id>", "name": "database/password", "value": {"password": "<the password>"}}`.
- MCP: `put_secret`.

The first write is version 1; each later write adds the next version. Two optional fields:

- `custom_metadata`, an object kept with the secret (non-empty metadata replaces what was there;
  leaving it out keeps it).
- `check_and_set`, the version you expect to be current. If another write got there first, lake
  answers `conflict` and writes nothing; `0` makes no check.

## Read a Secret

- REST: `GET /v1/read_secret?mount_id=<mount id>&name=database/password` reads the current version;
  add `&version=2` for another one.
- MCP: `read_secret` with `{"mount_id": "<mount id>", "name": "database/password", "version": 2}`.

## Inspect a Secret's History

1. On **Organization → Secrets**, press **Secrets** on the mount.
2. Enter the **Secret name** and press **Inspect history**.

The page shows the current version, the number of versions, how many are destroyed, the custom
metadata, and each version with its state (active, deleted or destroyed) and dates. It never shows a
value.

- REST: `GET /v1/secret_metadata?mount_id=<mount id>&name=database/password`.
- MCP: `secret_metadata`.

## Delete, Undelete or Destroy Versions

- Delete (reversible): `POST /v1/delete_secret_versions` with
  `{"mount_id": "<mount id>", "name": "database/password", "versions": [1, 2]}`.
- Undelete: `POST /v1/undelete_secret_versions` with
  `{"mount_id": "<mount id>", "name": "database/password", "versions": [2]}`.
- Destroy (permanent): `POST /v1/destroy_secret_versions` with
  `{"mount_id": "<mount id>", "name": "database/password", "versions": [1]}`.
- MCP: the tools of the same names.

A deleted version cannot be read until it is undeleted. A destroyed version can never be read or
undeleted again.

## Encryption Keys

On **Organization → Secrets**, press **Keys** on a mount. The **Encryption Keys** page lists the
mount's keys with their current version, whether each was derived from another, and when it was
created and updated. Key material is never returned.

Create a key:

- REST: `POST /v1/create_encryption_key` with `{"mount_id": "<mount id>", "name": "customer-data"}`.
- MCP: `create_encryption_key`.

Use it. Plaintext, messages and contexts are base64url text:

| Task | Call |
|---|---|
| Encrypt | `POST /v1/encrypt` |
| Decrypt | `POST /v1/decrypt` |
| Re-encrypt under the current version | `POST /v1/rewrap` |
| Sign (Ed25519) | `POST /v1/sign` |
| Verify a signature | `POST /v1/verify` |
| HMAC-SHA-256 | `POST /v1/hmac` |
| Rotate | `POST /v1/rotate_encryption_key` |
| Derive a child key | `POST /v1/derive_encryption_key` |

For example, `POST /v1/encrypt` with
`{"mount_id": "<mount id>", "name": "customer-data", "plaintext": "aGVsbG8"}` answers a ciphertext
of the form `lake:v1:<key id>:<version>:<sealed>`. Then `POST /v1/decrypt` with
`{"mount_id": "<mount id>", "name": "customer-data", "ciphertext": "<the ciphertext>"}` gives the
plaintext back.

`POST /v1/sign` with
`{"mount_id": "<mount id>", "name": "customer-data", "message": "aGVsbG8"}` answers a base64url
signature, and `POST /v1/verify` with
`{"mount_id": "<mount id>", "name": "customer-data", "message": "aGVsbG8", "signature": "<signature>"}`
answers whether it is valid and which key version signed it.

`POST /v1/rotate_encryption_key` with `{"mount_id": "<mount id>", "name": "customer-data"}` makes a
new current version. Encrypt always uses the current version; old ciphertext still decrypts, and
`POST /v1/rewrap` with
`{"mount_id": "<mount id>", "name": "customer-data", "ciphertext": "<the ciphertext>"}` moves it to
the current version without the plaintext leaving lake.

`POST /v1/derive_encryption_key` with
`{"mount_id": "<mount id>", "parent_name": "customer-data", "name": "customer-data-eu", "context": "ZXU"}`
stores a child key derived from the parent and the context.

## Dynamic Secret Leases

A lease generates a fresh username and password for a role, valid for 1 to 3,600 seconds (default
300):

- REST: `POST /v1/lease_dynamic_secret` with
  `{"mount_id": "<mount id>", "role": "reporting", "lifetime_seconds": 600}`. The value comes back
  once.
- Renew: `POST /v1/renew_dynamic_secret` with `{"lease_id": "<lease id>", "lifetime_seconds": 600}`.
- Revoke: `POST /v1/revoke_dynamic_secret` with `{"lease_id": "<lease id>"}`.
- MCP: the tools of the same names.

Renewal never shortens a lease, and a revoked or expired lease cannot be renewed (`conflict`).
Revocation erases the stored value at once; an expired lease is erased by lake shortly after it
ends.

## Rotate the Account Key

`POST /v1/rotate_customer_kek` with `{"customer_id": "<customer id>"}` makes a new account key and
rewraps every data key in every workspace that holds a mount. Secret values are not re-encrypted. If
a workspace is not ready, the rotation stops with `workspace_not_ready`, everything stays readable,
and the next rotation finishes it.

## Permissions

| Call | Needs |
|---|---|
| `create_secret_mount` | `admin` on the organization |
| `list_secret_mounts`, `get_secret_mount`, `read_secret`, `secret_metadata`, `list_encryption_keys`, `get_encryption_key` | `read` on the mount |
| `put_secret`, the version changes, every key operation, leases | `write` on the mount |
| `rotate_customer_kek` | `admin` on the account |

Grants on a mount are inherited through the organization like any other object
([Grants and Effective Access](/docs/access)).

## What Is Recorded

Every call is one signed audit event naming the caller, mount, workspace, secret name and version,
or key and version. Values, plaintext, ciphertext, signatures and MACs are never in the audit. Lease
lifecycle events are also appended to lake's transparency log ([Activity and Audit](/docs/activity)).

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| **Choose a workspace** on the Secret Mounts page | You have several workspaces; pick the one the mount was created in. |
| **No secret mounts** | None in that workspace that you can read. An organization admin creates them. |
| `invalid_input` at `/workspace_id` | The workspace is missing, unknown or not one of the account's. |
| `conflict` at `/name` | A mount with that name already exists in that workspace and organization. |
| `conflict` at `/check_and_set` | Another write changed the secret first. Read it again and retry. |
| `workspace_not_ready` | The mount's workspace Zetta is not running yet. See [Workspaces](/docs/workspaces). |
| `not_found` at `/lease_id` | The lease id is unknown or malformed. |
| `unauthorized` | You have no `read` or `write` grant on the mount. |

## Related Pages

- [Organizations, Members and Teams](/docs/organizations)
- [Grants and Effective Access](/docs/access)
- [Apps](/docs/apps)
- [Activity and Audit](/docs/activity)
- [Security Model](/docs/security)
