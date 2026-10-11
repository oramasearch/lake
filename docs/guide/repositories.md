# Repositories

A repository holds files and their whole history: every captured write, every named change and
every reference such as `main`. You work in it with the `liquid` command line, browse it in the
web app, review its changes, track issues and publish releases. This page covers creating a
repository, browsing it, cloning and pushing, and the lifecycle actions (rename, visibility,
delete).

## Concepts

| Term | What it is |
|---|---|
| **Repository** | Files and history, addressed as `<owner handle>/<name>`. Its id is a UUID; every call names it by that id. |
| **Owning workspace** | Every repository belongs to one workspace, chosen when it is created and never changed. Its description, issues, reviews, releases, webhooks and branch protection rules are kept in that workspace. |
| **Visibility** | `private` (the default: only people granted access), `organization` (members of the owning organization) or `public` (anyone, signed in or not, may read it). |
| **Change** | A named, reviewable set of captured writes, made with `liquid cut`. The Changes tab lists them. |
| **Reference** | A name pointing at a change, such as `main`. The first published reference is the repository's default. |
| **Tombstone** | What a deleted repository becomes: it stops working at once and is kept for 30 days before its data may be collected. |

`customer_id` in the calls below is the id of the account that owns your workspaces (yours, or
your organization's). [Getting Started](/docs) says where to find it and
your workspace id.

## Create a Repository

You need a workspace first: a repository cannot exist without one, and there is no default.

1. Open **Data → Repositories** and press **New repository**.
2. In the **New Repository** drawer choose the **Workspace** (your workspaces are listed first,
   then each organization's), type a **Repository name** (lowercase letters, numbers and inner
   hyphens) and, optionally, a **Description**.
3. Press **Create repository**. The repository's home page opens. New repositories are private.

If you have no workspace yet the drawer says **Create a workspace first** and links to
**Ship → Workspaces**.

- REST: `POST /v1/create_repository` with
  `{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "name": "analytics", "description": "Notebooks and data for the analytics team", "visibility": "private"}`.
  The answer is the repository, with its `id`.
- MCP: `create_repository` with the same input.

`visibility` may be set at creation to `private`, `organization` or `public`; the drawer always
creates a private one, and you change it afterwards.

## Find Your Repositories

**Data → Repositories** lists every repository you may read, newest first, with figures for how
many are private and public and how many workspaces they live in. Search by name and filter by
workspace and visibility. Each row names its **Owner**, **Workspace**, **Visibility** and
**Last change**.

Signed out, the same page is **Public Repositories** and shows only public ones.

- REST: `GET /v1/list_repositories?customer_id=<customer id>&workspace_id=<workspace id>&limit=50`.
  Both filters are optional: leave out `workspace_id` for every workspace of that account, and
  leave out both for everything you can read.
- MCP: `list_repositories`.
- One repository: `GET /v1/get_repository?id=<repository id>` (MCP `get_repository`).

A row whose description cannot be read (for example while its workspace is starting) shows
*Unavailable*; the rest of the list still works.

## Browse a Repository

Open a repository from the list. Its home page has tabs along the top:

| Tab | What it shows |
|---|---|
| **Files** | The Drive view of the repository: folders and files to browse, upload and share. Signed-in readers only. |
| **Code** | The home page: files, recent changes, the README, Clone and push, references, releases, languages and effective access. |
| **Changes** | The change log; open a change to review it ([Reviews, Issues and Releases](/docs/repository-collaboration)). |
| **Issues** | Issues and their discussion. |
| **Releases** | Tagged releases with their verified build artifacts. |
| **Artifacts** | Build results recorded in the repository. |
| **Deployments** | The repository's environments and their deployments ([Deployments and Environments](/docs/deployments)). Signed-in readers only. |
| **Webhooks** | Webhooks and their delivery attempts ([Branch Protection, Webhooks, Import and Export](/docs/repository-settings)). Signed-in readers only. |
| **Protection** | Branch protection rules and refused updates. Shown to the instance operator only. |

On the home page:

- **Browse files** opens the tree: every path at the reference, with its kind, mode, size and the
  newest change that touched it.
- Open a file to see it with line numbers. Click a line number for a link to that line; shift-click
  another to select a range (`#L5-L9`). Each line shows the change that last wrote it (blame).
  **Raw** shows the bytes as text and **Download** saves the exact file. C and C++ files are
  highlighted.
- The README is rendered as Markdown. Raw HTML in it is shown as text, never run.

The same reads are capabilities:

- `GET /v1/repository_tree?id=<repository id>&ref=main` (MCP `repository_tree`): every path at
  `main`.
- `GET /v1/repository_file?id=<repository id>&ref=main&path=README.md` (MCP `repository_file`):
  one file's contents.
- `GET /v1/list_changes?id=<repository id>` (MCP `list_changes`): the change log.
- `GET /v1/get_change?id=<repository id>&change_id=<change id>` and
  `GET /v1/change_diff?id=<repository id>&change_id=<change id>` (MCP `get_change`, `change_diff`):
  one change and its differences from its first parent.
- `GET /v1/repository_refs?id=<repository id>` (MCP `repository_refs`): the references and the
  change each points at.

## Clone and Push with liquid

Repositories are read and written with the `liquid` command line. The full set of commands is in
[The liquid CLI](/docs/cli); this is the path from the web app to a working checkout.

1. Make a token: open the account menu → **Tokens** → **New token**. For push and pull, give it
   the scopes `refs,find_missing,push,pull,upload_chunks,read_chunk` (the drawer fills them in with
   one click). Copy the secret; it is shown once.
2. In your terminal, export it: `export LIQUID_TOKEN=<the token>`. liquid sends it as the bearer
   credential and never prints it; it never goes in the URL.
3. On the repository's home page press **Clone** (or use the **Clone and push** card) and copy the
   first block. It looks like this:

```sh
liquid clone https://lake.oramadev.net/v1/repositories/<repository id> analytics
cd analytics
```

4. Work in the checkout. liquid captures your writes as you save; name them as a change with
   `liquid cut "describe the change"`.
5. Copy the second block to publish:

```sh
liquid push https://lake.oramadev.net/v1/repositories/<repository id>
```

`liquid pull <the same URL>` brings in what others pushed. The address is
`<lake origin>/v1/repositories/<repository id>`; the page fills in your Lake's origin. If the page
says the commands are unavailable, this Lake has no external origin configured, and the operator
must set one.

A token can also be made by API: `POST /v1/create_token` with
`{"name": "laptop liquid", "scopes": ["refs", "find_missing", "push", "pull", "upload_chunks", "read_chunk"]}`
(MCP `create_token`). See [Tokens and Keys](/docs/tokens).

## Change Visibility

There is no visibility control on the page; it is a capability.

- REST: `POST /v1/set_repository_visibility` with
  `{"id": "<repository id>", "visibility": "public"}`.
- MCP: `set_repository_visibility`.

Making a repository public lets anyone read its files, changes, issues, reviews and releases
without signing in. Webhooks, branch protection evidence and the Drive view still need a signed-in
reader.

## Rename a Repository

Renaming changes only the name in its address; the id, history and workspace stay.

- REST: `POST /v1/rename_repository` with `{"id": "<repository id>", "name": "analytics-next"}`.
- MCP: `rename_repository`.

Clones keep working because liquid addresses the repository by id.

## Move a Repository to Another Owner

A repository never changes owner or workspace. `transfer_repository` always answers `conflict`.
To move one, export it and import the archive into a workspace of the other owner
([Export and Import](/docs/repository-settings#export-a-repository)).

## Delete a Repository

Deleting tombstones the repository: it stops answering at once, its name is free again
immediately, and it is kept for 30 days before its data may be collected. A tombstoned repository
cannot be renamed or have its visibility changed. The home page shows a **Tombstoned** banner with
the retention date.

- REST: `POST /v1/delete_repository` with `{"id": "<repository id>"}`.
- MCP: `delete_repository`.

Deleting a workspace deletes every repository in it, with the same tombstone, as part of the
workspace deletion ([Workspaces](/docs/workspaces)).

## See Who Has Access

The home page's **Effective access** card lists who can reach the repository and how. **View
access details** opens the full table: each person or organization identity, its role, the actions
it allows, and whether the access is direct or inherited (for example from an organization
membership).

- REST: `GET /v1/entity_effective_access?object_type=repository&id=<repository id>`.
- MCP: `entity_effective_access`.

Grants themselves are covered in [Grants and Effective Access](/docs/access).

## Limits

- Names: lowercase letters, digits and inner hyphens, 1 to 256 characters, unique among the live
  repositories of one owner.
- Deleted repositories are kept for 30 days; there is no undelete capability.
- A single sync request body is at most 64 MiB.
- Storage counts against your account's storage limit ([Usage and Metering](/docs/usage)). When the
  limit is reached, pushes are refused and everything already stored stays readable.

## Permissions

| To | You need |
|---|---|
| Read a public repository | nothing; signed out works |
| Read a private or organization repository | `read` on it (a grant, an organization role or ownership) |
| Create a repository | `write` on the account that owns the workspace |
| Push, rename, change visibility, delete | `write` on the repository |
| See effective access | `share` on the repository |

A signed-out visitor who opens a private repository's address is sent to sign in, and is never told
whether it exists.

## Troubleshooting

| You see | Why, and what to do |
|---|---|
| `invalid_input` at `/workspace_id` | The workspace is missing, unknown or deleted. Pick a live workspace from **Ship → Workspaces**. |
| `invalid_input` at `/customer_id` | The workspace belongs to another account. Use the `customer_id` that owns it. |
| `conflict` at `/name` | A live repository with that name already exists for this owner. Choose another name. |
| `workspace_not_ready` | The workspace is still starting; the description cannot be written yet. Wait until **Ship → Workspaces** shows it ready. |
| `unauthenticated` on push or pull | `LIQUID_TOKEN` is missing, expired or revoked. Make a new token. |
| `unauthorized` on push | The token lacks a sync scope, or you lack `write`, or a branch protection rule refused the reference ([Branch Protection](/docs/repository-settings#branch-protection)). |
| Push answers 429 `busy` | The storage limit is reached. Delete what you no longer need or ask an administrator to raise the limit. |
| `store_unavailable` (503) | The workspace's store is being handed over; retry. |
| *Description unavailable* | The owning workspace cannot be read right now; everything else still works. |

## Related Pages

- [The liquid CLI](/docs/cli)
- [Reviews, Issues and Releases](/docs/repository-collaboration)
- [Branch Protection, Webhooks, Import and Export](/docs/repository-settings)
- [Sharing and Shared with Me](/docs/sharing)
- [Search](/docs/search)
- [Workspaces](/docs/workspaces)
- [Tokens and Keys](/docs/tokens)
