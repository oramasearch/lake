# Sharing and Shared with Me

Share a folder of a repository with a person, with an organization, or with anyone who holds a
link, and see the folders other people shared with you. Sharing works on the Drive view of a
repository (its **Files** tab), and every share is a policy grant lake checks on each open, so the
page always tells you who can reach a folder before and after you share it.

## Concepts

A folder can be delivered three ways:

| Delivery | What the recipient gets | Can you take it back? |
|---|---|---|
| **Live grant** (People, Organization) | The live folder, under its own identity. Later changes and moves reach them. | Yes: change the policy and the next open is refused. |
| **Link** | Anyone holding the link opens the live folder, signed in or not, until the link expires or is revoked. | Yes: revoking stops the link's next open. |
| **Independent copy** | Bytes someone downloaded. | No. A downloaded copy is not live and cannot be recalled by anything in lake. |

More terms:

| Term | What it is |
|---|---|
| **Role** | What a recipient may do: **View** (browse and download), **Edit** (also upload, rename, move and delete) or **Share** (also create shares). |
| **Email or handle** | How you name a person when you share: their sign-in email, or their handle (`@ana`). |
| **TAAP identity** | The id lake knows a person or organization by; programs may share by it. A person can show theirs with `liquid identity show`. |
| **Inherited access** | Access that comes from somewhere above the folder, such as the repository or an organization membership. The access table marks each entry **direct** or **inherited**. |
| **Holding workspace** | The workspace a folder's links are kept in. Links live in one workspace of the repository's owner, chosen when you open the sharing page. |
| **Stable folder identity** | A shared folder is followed by its identity, not its path, so moving or renaming it keeps the share pointing at the same folder. |

## Before You Share: Who Has Access

1. Open **Data → Repositories**, open the repository and choose the **Files** tab.
2. Go to the folder and press **Share**.
3. If the repository's owner has several workspaces and none is current, choose the workspace to
   **Hold this folder's links here**.
4. The page opens with figures (**With access**, **Active links**, **Expired or revoked**) and the
   table **Who Can Access This Folder Now**: each principal, its role, the actions it allows, and
   whether access is direct or inherited.

The table is the policy engine's own answer, the same check lake runs on every open.

- REST: `GET /v1/drive_effective_access?repository_id=<repository id>&workspace_id=<workspace id>&ref=main&path=reports`
- MCP: `drive_effective_access` with the same input. The answer also lists the folder's links held
  in that workspace.

## Share with a Person or an Organization

You name the person by their email or their handle; lake finds who that is and records the share
itself.

1. On the sharing page, under **Choose a Delivery**, press **Share with person** (or **Share with
   organization**). The header's **Share with person** does the same.
2. In **Person (email or handle)**, start typing: the list offers the people of this workspace's
   account (its owner, and for an organization every member) by email and handle. Choose one, or
   type an email (`ana@example.com`) or a handle (`@ana`). For an organization, type its handle in
   **Organization (handle)**.
3. Choose **What they can do**: **View — browse and download**, **Edit — upload, rename, move, and
   delete**, or **Share — browse and create shares**.
4. Press the dialog's **Share with person** (or **Share with organization**). The access table
   updates.

Lake writes a statement of the share (the folder, the person, the role and who shared it), signs
it with lake's own identity, and keeps the statement's digest on the grant as its evidence. You
never make or paste a digest. Sharing the same folder with the same person and role again answers
the grant that is already there.

Someone outside the account cannot be found by email: invite them to the organization first
([Organizations, Members and Teams](/docs/organizations)), then share.

An organization grant goes to that organization identity; the people who then have access appear
in the table only as their own access arrives.

- See who can be named: `GET /v1/drive_share_people?repository_id=<repository id>&ref=main&path=reports`
  (MCP: `drive_share_people`). Each person has `taap_id`, `kind`, `display_name`, `email` and
  `handle`.
- REST: `POST /v1/share_drive_folder` with
  `{"repository_id": "<repository id>", "ref": "main", "path": "reports", "recipient": "ana@example.com", "role": "read"}`.
  `recipient` is an email or a handle; `role` is `read`, `write` or `share`. The answer carries the
  grant, the person, and the signed statement with its `digest`.
- A program that already holds a person's TAAP identity sends it instead of `recipient`:
  `{"repository_id": "<repository id>", "ref": "main", "path": "reports", "principal_taap_id": "<recipient TAAP id>", "role": "read"}`.
  Lake refuses an identity it does not know (`invalid_input` at `/principal_taap_id`).
- `attestation_digest` is optional and only for repeating a share: lake accepts only the digest it
  answered for the same folder, person and role, and refuses any other (`invalid_input` at
  `/attestation_digest`).
- MCP: `share_drive_folder` with the same input.

## Create a Link

1. On the sharing page press **Create link**.
2. Choose **What link holders can do**: **View — browse and download**, or **Write scope —
   capability-aware clients only**. The signed-out page a link opens is always read-only.
3. Set **Until when (UTC)**: every link needs a future expiry.
4. Press **Create link**. Under **Copy this link now** the page shows the link's address once;
   lake keeps only a hash of it, so copy it before you leave.

Links have no passcode. The **Passcode** field says so; nothing is sent or stored.

- REST: `POST /v1/create_drive_share_link` with
  `{"repository_id": "<repository id>", "workspace_id": "<workspace id>", "ref": "main", "path": "reports", "role": "read", "expires_at": "2026-12-31T23:59:59Z"}`.
- MCP: `create_drive_share_link` with the same input. The answer carries the token once.

The link's address is `/share/<token>` on your lake. The token names the workspace that holds it,
so it works on its own, without a lookup anywhere else.

## Manage and Revoke Links

**Folder Share Management** on the sharing page lists every link on the folder held in that
workspace, with its audience, allowed use, expiry and state (active, expired or revoked). It names
the holding workspace and links to the owner's other workspaces, where other links may be held.

1. Press **Revoke link** on a row, or open the link's own page and use **Revoke this link** →
   **Revoke link**.
2. Confirm. The link stops opening the folder on its next use, including a download clicked from a
   page opened before the revocation.

- REST: `POST /v1/revoke_drive_share_link` with
  `{"workspace_id": "<workspace id>", "link_id": "<link id>"}`.
- MCP: `revoke_drive_share_link` with the same input.

Revoking or expiring a link does not recall files someone already downloaded.

## Open a Link

A recipient opens `/share/<token>` in a browser. No account or sign-in is needed.

- Folders open with **Open folder** or their name; **Up one folder** and **Shared folder** move back
  up. Navigation stops at the shared folder: its parent and siblings are refused.
- A file opens in a preview (Markdown, text, CSV, JSON, images, PDF, video and audio); **Back to
  folder** returns. A format without a preview offers its download.
- **Download** saves the exact stored bytes.

Every click checks the link's role, scope, expiry and revocation again.

The same reads, for scripts, need no credential:

- `GET /v1/resolve_drive_share_link?token=<token>&ref=main` (MCP `resolve_drive_share_link`): the
  shared folder's children; add `path` for a folder below it.
- `GET /v1/preview_drive_share_link?token=<token>&ref=main&path=reports/summary.md` (MCP
  `preview_drive_share_link`): one file's preview.
- `GET /v1/download_drive_share_link?token=<token>&ref=main&path=reports/summary.md` (MCP
  `download_drive_share_link`): the file's bytes as base64 in `content_base64`.

## Shared with Me

**Data → Shared with Me** lists the folders shared directly with your identity: each **Folder**,
who **Shared by**, **What you can do**, and its **State** (**Available**, or **Removed** when the
folder no longer exists at `main`). Search by name and filter by state.

It lists live grants only. A link someone sent you is not listed, and neither is access you have
through the whole repository or an organization.

- REST: `GET /v1/drive_shared_with_me`
- MCP: `drive_shared_with_me` (no input).

## Limits

- Links need a future expiry in UTC, written as an RFC 3339 time.
- A link is held in one workspace and can be listed and revoked only through that workspace.
  Deleting the workspace deletes its links.
- No passcodes on links.
- The signed-out link pages are read-only whatever the link's role.
- Shared with Me covers folders shared with you directly, by identity.

## Permissions

| To | You need |
|---|---|
| Open the sharing page, see effective access, share, create or revoke links | `share` on the folder (from the repository, an organization role, or a **Share** grant) |
| Open a link | only the link |
| See Shared with Me | to be signed in |

People without `share` who open the sharing page see the refusal and nothing else.

## Troubleshooting

| You see | Why, and what to do |
|---|---|
| `unauthorized` on the sharing page | You may read the folder but not share it. Ask someone with **Share** on it. |
| `invalid_input` at `/workspace_id` | The workspace is not a live workspace of the repository's owner. Choose one from the page. |
| `invalid_input` at `/expires_at` | The expiry is not in the future. |
| `not_found` at `/link_id` when revoking | The link is held in another workspace. Open the sharing page with that workspace. |
| A recipient's link says the share is not recognized | The link was revoked, has expired, or its workspace was deleted. Create a new one. |
| A recipient cannot open a subfolder | That subfolder has its own, narrower policy; links never widen it. |
| `workspace_not_ready` | The holding workspace is starting; retry once it is ready. |

## Related Pages

- [Repositories](/docs/repositories)
- [Grants and Effective Access](/docs/access)
- [Organizations, Members and Teams](/docs/organizations)
- [Security Model](/docs/security)
- [Activity and Audit](/docs/activity)
