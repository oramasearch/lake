# Secrets Pages

**What.** Organization → Secrets manages secret mounts, their secrets and versions, and their
encryption keys in the web app (lake#561). Before this, the pages were read-only and every write was
an API call. Each page calls only the capabilities in [../api/secrets.md](../api/secrets.md) and
[../api/encryption.md](../api/encryption.md), so it can do nothing the API cannot, and every action
is authorized and audited as the same call over REST or MCP would be.

## The Pages

| Route | File | What it does | Capabilities |
|---|---|---|---|
| `/secret-mounts?workspace_id=&organization_id=` | `frontend/src/pages/admin/SecretMountsPage.tsx` | The readable mounts of one workspace, with search. An **Owner** column names each mount's owner (the account, or the organization by name). **New mount** opens a modal: the workspace (chosen once, never changed), the **Owner**, a lowercase name and an optional description; it then opens the new mount | `list_secret_mounts`, `create_secret_mount`, `list_organizations`, `list_memberships`, `get_organization`, `get_customer` |
| `/secrets?mount_id=` | `SecretsPage.tsx` (`MountView`) | The mount's secrets: path, state (live or deleted), current version, updated and created; search and a Live/Deleted filter; pages of 200 with Previous and Next. **New secret** opens a drawer | `get_secret_mount`, `list_secrets`, `put_secret` |
| `/secrets?mount_id=&name=` | `SecretsPage.tsx` (`SecretView`) | One secret: its state, figures (current version, live, deleted, destroyed), custom metadata and version history. **New version**, **Reveal value**, and per version Reveal, Delete, Undelete and Destroy | `secret_metadata`, `put_secret`, `read_secret`, `delete_secret_versions`, `undelete_secret_versions`, `destroy_secret_versions` |
| `/encryption-keys?mount_id=` | `EncryptionKeysPage.tsx` | The mount's keys. **New key** (a new key, or one derived from another key and a context), **Rotate** per key, and **Try it**, a drawer for encrypt, decrypt, rewrap, sign, verify and HMAC | `list_encryption_keys`, `create_encryption_key`, `derive_encryption_key`, `rotate_encryption_key`, `encrypt`, `decrypt`, `rewrap`, `sign`, `verify`, `hmac` |

The value editor and the masked value are in `secretParts.tsx`; the pure rules (rows to a JSON
object, raw JSON, version state, base64url) are in `secretValue.ts`, and who may own a mount is in
`secretOwners.ts`, both tested by `frontend/tests/secrets.test.ts`.

## Choosing the Owner

A mount's owner is the account that owns its workspace or an organization inside that account
([../api/secrets.md](../api/secrets.md#who-owns-a-mount-lake561)). The New Mount modal's **Owner**
field offers, first and by default, the workspace's owner: **Your account** for the person's own
workspaces (another account's workspace shows that account's name), or the organization, marked
*(workspace owner)*, when the workspace is an organization's (`get_customer` names it). Then it
offers each organization the person administers (an `owner` or `admin` membership of the
organization itself, from `list_memberships`; the operator administers every one), except one whose
own space is another account's (`get_organization`'s `customer_id`). Choosing the account sends no
`organization_id`; choosing an organization sends its id. Nothing blocks a person who belongs to no
organization: their account is always offered. Changing the workspace returns the field to that
workspace's owner. A refusal at `/organization_id` or `/customer_id` shows under the field.

The mount's page names the owner in its facts the same way (**Owner**).

## Writing a Secret

The **New secret** drawer takes a path and a value. The value is one JSON object, written either as
**Key and value** rows (each value a string, typed into a password field) or as raw **JSON** (any
JSON object, including numbers and nested objects). Switching carries what is written across when it
can; a value with non-string fields stays in the JSON editor. A row with a value but no key, a key
used twice, or JSON that is not an object is refused before anything is sent. Custom metadata is an
optional JSON object under a disclosure.

- A path that is already listed becomes that secret's next version, checked against the version the
  list showed (`check_and_set`), and the drawer says so before saving.
- **New version** on a secret's page always sends `check_and_set` with the current version. If
  someone wrote a newer one meanwhile, lake answers `conflict` at `/check_and_set`, nothing is
  written, and the drawer says to look at the new version and write again.
- After saving, the drawer closes and a toast names the new version; a new secret opens its page.
  The value is never shown back.

## Revealing a Value

**Reveal value** (the current version) and **Reveal** (any live version) each run one `read_secret`
on that click. The answer is held in the page's state only: never in the read cache, the URL, a log
or browser storage. Each field is masked until **Show**; each has a copy button, and **Copy JSON**
copies the whole object. **Hide** forgets it; so do leaving the page, opening another secret,
writing a new version, and deleting or destroying the revealed version. Every reveal is an audited
`read_secret`.

## Delete, Undelete and Destroy

- **Delete** asks once: the version becomes unreadable, its data kept, and it can be undeleted.
- **Undelete** runs at once (it is reversible).
- **Destroy** asks in a dialog that names what is lost forever (the version's value, which can never
  be read or undeleted again) and keeps **Destroy for good** disabled until the secret's path is
  typed exactly: `ConfirmButton` with `typed` (`frontend/src/lib/overlay.tsx`).

A destroyed version reads *erased* and offers nothing.

## Encryption Keys

**New key** takes a lowercase name. The schema has no key type: one key encrypts and decrypts
(AES-256-GCM), signs and verifies (Ed25519), and computes HMAC-SHA-256. **Derive from** an existing
key asks for a context, sent as base64url, and calls `derive_encryption_key`. **Rotate** asks once,
naming the version that becomes current and that older versions still decrypt and verify.

**Try it** runs one operation against the key. Text typed into it is sent as unpadded base64url, as
the capabilities take it; a decrypted result is shown as text when it is UTF-8, else as base64url.
Encrypt and rewrap fill the ciphertext for a following decrypt; sign fills the signature for a
following verify. Sign, verify and HMAC take an optional key version. Nothing typed or answered is
kept once the drawer closes.

## Empty States

Each list's empty state leads to its create action: **New mount**, **New secret** and **New key**.

## How to Verify

```sh
cd frontend
npx tsc --noEmit -p .
npm test            # tests/secrets.test.ts among them
npm run build
```

`SecretsCapabilities.ListSecretsAnswersNamesAndStateNeverValues` and
`PostgresCapabilities.SecretsAndEncryptionRoundTrip` cover `list_secrets`;
`Stores/WorkspaceRecordStores.SecretMountsBelongToAnAccountOrAnOrganization` covers a mount an
individual account owns, and `secrets.test.ts` the Owner choices. In a browser, the pages'
empty and error states render without a workspace Zetta; a mount's secrets need one.

## How to Change

Keep the rule that a value is held only in a component's state. A new action on these pages is a
capability call through `act` (which refreshes the reads on screen); a new read of a value uses
`read` into state, never `useRead`, so it does not enter the shared read cache.
