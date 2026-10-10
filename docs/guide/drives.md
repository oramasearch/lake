# Drives

**Data → Drives** is where a workspace keeps its files and folders. Each drive keeps its files one
of three ways, chosen when you create it: as **workspace files**, in a **repository** with
history, or as **shortcuts** to things that already live somewhere else (a virtual drive). Every
drive opens in the same file manager: folders, uploads with progress, drag to move, rename, delete.

## Concepts

- **Drive.** A named place for files in one workspace. Its name is unique in the workspace. Other
  workspaces never see it, and deleting the workspace deletes its drives.
- **Workspace files.** Files and folders kept in the workspace Zetta, encrypted with a key that
  belongs to that workspace alone. Moving or renaming a file changes only its record: the bytes
  are never copied. Workspace files have **no history**: a file you replace or delete is gone.
- **Repository drive.** A drive whose files are a repository of the workspace. Every upload, move
  and delete is a change you can compare and restore, and folders can be shared
  ([Sharing and Shared with Me](/docs/sharing)). A new repository drive gets its first change when
  it is created, so it is ready at once. Deleting the drive never deletes its repository.
- **Virtual drive.** Folders of shortcuts only.
- **Shortcut.** A link in a folder to something that lives elsewhere: a repository, a file or
  folder in a repository, an item of another drive, a dataset, a transcript, a project, an agent,
  a model or a web address. Deleting a shortcut never deletes what it opens. A shortcut whose
  repository, path, drive item or transcript is gone is marked **Broken**; lake does not check
  datasets, projects, agents, models or web addresses.

## The Drives Page

**Data → Drives** shows four figures (Drives, Workspace files, Repositories, Virtual), a filter
box and chips for each kind, and a card per drive. **Recent Files** lists the workspace files
changed most recently across every workspace-files drive; each opens the folder that holds it.

## Create a Drive

1. Press **New drive**.
2. Type its **Name**.
3. Choose how it **Keeps its files as**: Workspace files, Repository or Virtual.
4. For a repository drive, choose **Create a new one** (the repository's name starts from the
   drive's; lowercase letters, numbers and hyphens) or **Use one of this workspace**.
5. Press **Create drive**. The drive opens.

REST: `POST /v1/create_drive` with `customer_id`, `workspace_id`, `name`, `storage` (`files`,
`repository` or `virtual`) and, for a repository drive, `repository_id` or `repository_name`.
MCP: `create_drive`. `list_drives` and `get_drive` read them.

## Work with Files

Open a drive from its card. The path above the list opens any folder above this one.

- **Upload.** Press **Upload** and choose files, or drop files anywhere on the list. To upload a
  folder with everything in it, choose **Upload a folder** in the drive's menu (**⋯**), or drop
  the folder. Its subfolders are made as needed. A banner shows the progress. In a workspace-files
  drive a file of any size uploads in 8 MiB pieces; a repository drive's upload is limited to what
  one request carries (64 MiB with encoding).
- **New folder.** Press **New folder** and type its name.
- **Open and download.** A folder's name opens it. A file's name opens it in the browser when the
  browser can show it (images, PDF, text, audio and video), and downloads it otherwise; **Download**
  in its menu always downloads. In a repository drive a file opens in the repository's preview.
- **Select.** Tick items, or click a row; **Shift**-click selects every row between, and **Ctrl**
  or **⌘**-click adds one. The selection bar moves or deletes them together.
- **Move.** Drag items onto a folder in the list, or onto a folder in the path above it. Or
  choose **Move** in an item's menu (or the selection bar), open the folder to move into, and
  press **Move here**. A folder never moves into itself.
- **Rename** and **Delete** are in each item's menu. Deleting a folder deletes everything in it.
  In a workspace-files or virtual drive this cannot be undone; in a repository drive it is a new
  change, and earlier versions stay in history.

For a repository drive, the drive's menu also has **History**, **Share this folder** and **Open
the repository**, which open the repository's own pages.

| Step | Workspace files or virtual drive (MCP tool and REST call) | Repository drive |
|---|---|---|
| List a folder | `list_drive_entries` (`GET`, `folder_id` or `path`) | `drive_browse` |
| New folder | `create_drive_folder_entry` (`path` may name several levels) | `create_drive_folder` |
| Upload small files | `upload_drive_entries` (at most 16 MiB a call) | `upload_drive_files` |
| Upload a large file | `begin_drive_upload`, `append_drive_upload` (8 MiB pieces), `finish_drive_upload` | — |
| Read a file | `read_drive_file` (16 MiB a call), or `GET /drive/files/download` | `download_drive_file`, or `GET /drive/download` |
| Move or rename | `move_drive_entry` (`parent_id`, `name`) | `move_drive_path` |
| Delete | `delete_drive_entry` | `delete_drive_path` |

Every REST call above takes `customer_id` and `workspace_id` with the drive's `drive_id`; a
repository drive's calls take its `repository_id` and `ref` (`main`) instead.

## Add a Shortcut

1. In a workspace-files or virtual drive, press **New shortcut**.
2. Choose what it **Opens**, then choose or type it: a repository from the workspace's
   repositories; a path in one (such as `reports/2026/q3.pdf`); a drive, then **Choose the item**;
   a transcript; a dataset as `<database>/<dataset>`; a project's name; an agent's or a model's
   id; or a web address starting with `http://` or `https://`.
3. Check its **Name** (it starts from what you chose) and press **Create shortcut**.

A repository, path, drive item or transcript must exist when the shortcut is made. REST:
`POST /v1/create_drive_shortcut` with `drive_id`, `name`, `parent_id` (the folder, empty for the
top) and `target` (`{kind, ...}`). MCP: `create_drive_shortcut`.

## Rename or Delete a Drive

In the drive, choose **Drive settings** in its menu (**⋯**). **Rename drive** gives it a new name.
To delete it, type its name and press **Delete drive**: a workspace-files drive's folders and files
go with it and cannot be restored; a virtual drive's shortcuts go, never what they open; a
repository drive's repository stays, with its history. REST: `rename_drive`, and `delete_drive`
with `confirm_name`. MCP: the same names.

## Limits

- Workspace files have no version history, no sharing links and no previews rendered by lake; a
  repository drive has all three.
- One upload of a workspace file is at most 64 GiB. A finished upload is checked: lake reads the
  file back and records its SHA-256.
- An upload left unfinished is never listed; delete it with `delete_drive_entry` by its id, or
  start it again.
- A drive's names are unique in the workspace, an item's within its folder. An upload whose name
  is taken in the folder gets the next free name (`notes (2).txt`) unless the call asks to
  `replace` or `refuse`.

## Permissions

Reading a drive needs read access to the workspace's account; creating, changing and deleting
needs write access. A repository drive's files also follow the repository's own access and folder
sharing ([Grants and Effective Access](/docs/access)).

## Troubleshooting

- **"a repository drive's files are browsed and written with drive_browse"**: the call named a
  repository drive; use the repository capabilities with its `repository_id`.
- **"a virtual drive holds folders and shortcuts only"**: upload into a workspace-files drive.
- **"the upload is not complete"**: `finish_drive_upload` came before every piece was sent; send
  the missing pieces, in order, then finish again.
- **"this chunk does not continue the upload"**: a piece was sent out of order. Send each piece at
  the offset where the previous one ended; a piece sent twice is accepted.
- **"this lake keeps no workspace files"**: this lake has no workspace Zetta. Use a repository
  drive.

## Related Pages

- [Repositories](/docs/repositories)
- [Sharing and Shared with Me](/docs/sharing)
- [Transcripts and Speech-to-Text](/docs/transcripts)
- [Workspaces](/docs/workspaces)
