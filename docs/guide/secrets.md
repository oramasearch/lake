# Secrets and Encryption Keys

Lake keeps your secrets (passwords, API keys, connection strings) and encryption keys, for your
own account or for an organization. Secrets are versioned JSON values you manage in the web app, through the REST API or over MCP.
Encryption keys never leave Lake: you send data to Lake and get back ciphertext, a signature or an
HMAC.

The web app lists **metadata**: which mounts exist, which secrets a mount holds, a secret's version
history, and a key's versions. A secret's value appears only when you press **Reveal**, masked until
you show it; key material is never shown anywhere.

## Concepts

| Term | What it is |
|---|---|
| **Secret mount** | A named container of secrets and keys that lives in one workspace, chosen when it is made and never changed. Its **owner** is the account that owns the workspace (your own account for your workspaces) or an organization you administer in that account. Access is granted on the mount. |
| **Secret** | A name in a mount (such as `database/password`) holding a JSON object, with an immutable history of versions. |
| **Version** | One value of a secret. Writing again creates the next version; numbers are never reused. |
| **Delete, undelete, destroy** | Delete hides a version but keeps it (reversible); undelete brings it back; destroy erases its data for good, keeping only its history row. |
| **Encryption key** | A named, versioned key in a mount, used for encrypt and decrypt, sign and verify, HMAC and derivation. Rotation adds a version; old versions still decrypt and verify. |
| **Dynamic secret lease** | A generated username and password for a role, returned once and valid for a limited time, renewable and revocable. |
| **Account key (KEK)** | The key that wraps every mount's data keys for one account. It can be rotated. |

Everything about a mount (its secrets, versions and keys) is sealed in that workspace.
Deleting the workspace deletes its mounts.

## Find Your Secret Mounts

1. Open **Organization → Secrets**. The **Secret Mounts** page lists the mounts you can read in one
   workspace.
2. If you have more than one workspace, choose it in **Workspace**.
3. Each mount has **Secrets** and **Keys** buttons.

Each mount's page lists its secrets by path, with their state (live or deleted), current version and
when they changed. Search the paths, or filter **Live** or **Deleted**.

- REST: `GET /v1/list_secrets?mount_id=<mount id>&limit=100`; add `&prefix=database/` for one group,
  and pass the answer's `next_cursor` as `&cursor=` for the next page.
- MCP: `list_secrets`. It never returns a value.

- REST: `GET /v1/list_secret_mounts?customer_id=<customer id>&workspace_id=<workspace id>&limit=100`.
- MCP: `list_secret_mounts` with the same input.

## Create a Mount

Anyone who administers the workspace's account creates mounts there: you, in your own workspaces,
and an organization's owners and admins in the organization's workspaces. You need no organization
to keep secrets.

1. Open **Organization → Secrets** and press **New mount**.
2. Choose the **Workspace**. The mount's secrets and keys are sealed in that workspace, and it cannot
   be changed later.
3. Choose the **Owner**. It starts on the workspace's owner: **Your account** in your own workspaces,
   the organization in an organization's workspaces. An organization you administer inside that
   account is offered too.
4. Enter a **Name** (lowercase letters, digits and dashes) and, optionally, a **Description**.
5. Press **Create mount**. Lake opens the new mount.

For a mount your account owns, leave out `organization_id`; `customer_id` is the id of the account
that owns the workspace:

- REST: `POST /v1/create_secret_mount` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "payments", "description": "Payment service credentials"}`.
- MCP: `create_secret_mount` with the same input.

For a mount an organization owns, add its id:

- REST: `POST /v1/create_secret_mount` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "organization_id": "<organization id>", "name": "payments"}`.

The answer names the owner: `owner_type` is `customer` for an account's mount (its
`organization_id` is `null`) and `organization` for an organization's. The name is lowercase and
unique within its workspace and owner, so your account and an organization may each have a mount
called `payments` in the same workspace. The answer's `id` is the `mount_id` every other call
takes; none of them takes `workspace_id`, because the mount names its workspace.

For example, with a key in `LAKE_KEY` ([REST API and OpenAPI](/docs/developer)), create a mount
your account owns, write a secret into it and read it back:

```
curl -s https://lake.oramadev.net/v1/create_secret_mount \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "payments"}'
curl -s https://lake.oramadev.net/v1/put_secret \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"mount_id": "<mount id>", "name": "database/password", "value": {"password": "<the password>"}}'
curl -s "https://lake.oramadev.net/v1/read_secret?mount_id=<mount id>&name=database/password" \
  -H "Authorization: Bearer $LAKE_KEY"
```

`<mount id>` is the `id` the first call answers. Replace `lake.oramadev.net` with your Lake's
address.

## Write a Secret

1. Open the mount and press **New secret**.
2. Enter the **Path**, such as `database/password`. Slashes group secrets.
3. Write the **Value**: under **Key and value**, one row per field (press **Add a field** for more);
   under **JSON**, any JSON object, including numbers and nested objects.
4. Optionally open **Custom metadata** and write a JSON object, such as an owner.
5. Press **Save secret**. The secret's page opens; the value is not shown back.

To write the next version, open the secret, press **New version**, write the value and press **Save
version**. The page checks against the current version, so if someone else wrote a newer one first,
nothing is written and the page asks you to look at it and write again. A **New secret** whose path
already exists becomes that secret's next version, checked the same way.

- REST: `POST /v1/put_secret` with
  `{"mount_id": "<mount id>", "name": "database/password", "value": {"password": "<the password>"}}`.
- MCP: `put_secret`.

The answer is the new version's metadata (`version`, `created_at`, `custom_metadata`), never the
value, so a script's log or an agent's transcript does not keep it. Read it back with `read_secret`.

The first write is version 1; each later write adds the next version. Two optional fields:

- `custom_metadata`, an object kept with the secret (non-empty metadata replaces what was there;
  leaving it out keeps it).
- `check_and_set`, the version you expect to be current. If another write got there first, Lake
  answers `conflict` and writes nothing; `0` makes no check.

## Read a Secret

1. Open the secret and press **Reveal value** for the current version, or **Reveal** on a version in
   **Version history**.
2. The value appears masked. Press **Show** to see it, the copy button beside a field to copy it,
   or **Copy JSON** for the whole object.
3. Press **Hide** when you are done. Leaving the page also forgets the value; it is never stored in
   the browser.

Every reveal is an audited read.

- REST: `GET /v1/read_secret?mount_id=<mount id>&name=database/password` reads the current version;
  add `&version=2` for another one.
- MCP: `read_secret` with `{"mount_id": "<mount id>", "name": "database/password", "version": 2}`.

## Inspect a Secret's History

1. On **Organization → Secrets**, press **Secrets** on the mount.
2. Press the secret's path in the list.

The page shows the current version, how many versions are live, deleted and destroyed, the custom
metadata, and each version with its state (live, deleted or destroyed) and dates.

- REST: `GET /v1/secret_metadata?mount_id=<mount id>&name=database/password`.
- MCP: `secret_metadata`.

## Delete, Undelete or Destroy Versions

On the secret's page, each version in **Version history** has its actions:

- **Delete** asks once, then hides the version. Its data is kept.
- **Undelete** brings a deleted version back at once.
- **Destroy** erases the version's value for good. The dialog names what is lost and asks you to type
  the secret's path before **Destroy for good** is enabled.

Through the API:

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

1. Press **New key** and enter a **Name** (lowercase letters, digits and dashes). One key encrypts,
   signs and computes HMACs; there is no key type to choose.
2. To derive a child of another key instead, choose it in **Derive from** and enter a **Context**
   (text such as a region), then press **Derive key**. Otherwise press **Create key**.

Rotate a key: press **Rotate** on its row. The dialog names the version that becomes current; older
versions still decrypt and verify.

Try a key: press **Try it** on its row, choose **Encrypt**, **Decrypt**, **Rewrap**, **Sign**,
**Verify** or **HMAC**, enter the text, and press the button. The page sends text as base64url for
you. Encrypt fills the ciphertext for a following decrypt, and sign fills the signature for a
following verify. Nothing you type there is kept.

Through the API:

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
the current version without the plaintext leaving Lake.

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
Revocation erases the stored value at once; an expired lease is erased by Lake shortly after it
ends.

## Rotate the Account Key

`POST /v1/rotate_customer_kek` with `{"customer_id": "<customer id>"}` makes a new account key and
rewraps every data key in every workspace that holds a mount. Secret values are not re-encrypted. If
a workspace is not ready, the rotation stops with `workspace_not_ready`, everything stays readable,
and the next rotation finishes it.

## Permissions

| Call | Needs |
|---|---|
| `create_secret_mount` | `admin` on its owner: the account that owns the workspace, or the organization named by `organization_id` |
| `list_secret_mounts`, `get_secret_mount`, `list_secrets`, `read_secret`, `secret_metadata`, `list_encryption_keys`, `get_encryption_key` | `read` on the mount |
| `put_secret`, the version changes, every key operation, leases | `write` on the mount |
| `rotate_customer_kek` | `admin` on the account |

Grants on a mount are inherited through its owner like any other object: an account's mount from
the account, an organization's from the organization
([Grants and Effective Access](/docs/access)). Nobody else can read or change a mount your account
owns until you grant them access on it.

## What Is Recorded

Every call is one signed audit event naming the caller, mount, workspace, secret name and version,
or key and version. Values, plaintext, ciphertext, signatures and MACs are never in the audit. Lease
lifecycle events are also appended to Lake's transparency log ([Activity and Audit](/docs/activity)).

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| **Choose a workspace** on the Secret Mounts page | You have several workspaces; pick the one the mount was created in. |
| **No secret mounts** | None in that workspace that you can read. Press **New mount** to create one for your account. |
| **Someone wrote a newer version** | Another write changed the secret while you edited. Close the drawer, look at the new version, and write again. |
| `invalid_input` at `/workspace_id` | The workspace is missing, unknown or not one of the account's. |
| `conflict` at `/name` | A mount with that name already exists in that workspace for that owner. |
| `invalid_input` at `/organization_id` | The organization is not inside the account that owns the workspace. Choose that account as the owner, or an organization of it. |
| `conflict` at `/check_and_set` | Another write changed the secret first. Read it again and retry. |
| `workspace_not_ready` | The mount's workspace is not running yet. See [Workspaces](/docs/workspaces). |
| `not_found` at `/lease_id` | The lease id is unknown or malformed. |
| `unauthorized` | You have no `read` or `write` grant on the mount. |

## Related Pages

- [Organizations, Members and Teams](/docs/organizations)
- [Grants and Effective Access](/docs/access)
- [Apps](/docs/apps)
- [Activity and Audit](/docs/activity)
- [Security Model](/docs/security)
