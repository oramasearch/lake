# Drives

**Status: implemented 2026-10-10 in the source tree (lake#561); not deployed.** A workspace's
drives are the places it keeps files. Each drive keeps its files one of four ways (`storage`):

| `storage` | Files are | History | Written with |
|---|---|---|---|
| `repository` | a liquid repository of the workspace | every write is a change | the drive_* capabilities ([drive.md](../ui/drive.md)) on the drive's `repository_id` |
| `files` | workspace files: records and sealed objects in the workspace Zetta | none | the entry capabilities below |
| `virtual` | folders and shortcuts only | none | the entry capabilities below, without uploads |
| `store` | one of the workspace's bucket or filesystem stores, in place (lake#656) | the provider's | the entry capabilities below, an item's `id` being its path ([Store drives](#store-drives)) |

A drive is one sealed record in `lake_drives` of its workspace Zetta, found by its account; its
folders, files and shortcuts (a `files` or `virtual` drive's) are sealed records in
`lake_drive_entries`, found by drive ([workspace-records.md](../architecture/workspace-records.md)).
Nothing about a drive is in PostgreSQL. Deleting the workspace deletes them with its Zetta.

## Capabilities

Every capability takes `customer_id` and `workspace_id` (`Registry::call` checks the workspace is
the account's) and authorizes on the account: reads need `read`, actions `write`. Every call is
audited in the workspace's own log ([audit.md](audit.md#which-log)).

| Capability | Kind | Input (besides `customer_id`, `workspace_id`) | Output |
|---|---|---|---|
| `create_drive` | action | `name`, `storage`, `repository_id?`, `repository_name?`, `store_id?` | the drive |
| `list_drives` | read | — | `{drives: [drive]}`, by name |
| `get_drive` | read | `id` | the drive |
| `rename_drive` | action | `id`, `name` | the drive |
| `delete_drive` | action | `id`, `confirm_name` | `{deleted: true, entries_removed, repository_id?, store_id?}` |
| `list_drive_entries` | read | `drive_id`, `folder_id?` or `path?` | `{drive, folder_id, path, ancestors: [{id, name}], items: [entry]}` |
| `get_drive_entry` | read | `drive_id`, `id` | the entry |
| `create_drive_folder_entry` | action | `drive_id`, `parent_id?`, `path` | the last folder |
| `create_drive_shortcut` | action | `drive_id`, `parent_id?`, `name`, `target` | the entry, with `status` |
| `move_drive_entry` | action | `drive_id`, `id`, `parent_id?`, `name?` | the entry |
| `delete_drive_entry` | action | `drive_id`, `id` | `{deleted: true, entries_removed}` |
| `begin_drive_upload` | action | `drive_id`, `parent_id?`, `path`, `size`, `content_type?`, `on_conflict?` | `{entry, block_size, max_chunk_bytes}` |
| `append_drive_upload` | action | `drive_id`, `id`, `offset`, `content` (base64) | `{received, complete, size}` |
| `finish_drive_upload` | action | `drive_id`, `id`, `sha256?` | the entry |
| `upload_drive_entries` | action | `drive_id`, `parent_id?`, `on_conflict?`, `files: [{path` or `name, content, content_type?}]` | `{items: [entry]}` |
| `read_drive_file` | read | `drive_id`, `id`, `offset?`, `length?` | `{id, name, size, content_type, sha256, offset, content_base64}` |
| `recent_drive_entries` | read | `limit?` (1 to 100, default 20) | `{items: [{drive: {id, name}, entry}]}` |
| `load_drive_dataset` | action | `drive_id`, `id`, `table_name`, `format?`, `if_exists?` | `upload_dataset`'s answer, with `source: {drive_id, id, name}` |
| `load_drive_model` | action | `drive_id`, `id`, `name` or `version_of`, `format?`, `feature_names?`, `kind?`, `metrics?` | `register_model`'s answer (`model_id`, …), with `source: {drive_id, id, name}` ([models.md](models.md#a-standalone-model-lake654)) |

| `search_drives` | read | `q` (1 to 255 bytes), `drive_id?`, `limit?` (1 to 200, default 50) | `{items: [match], truncated, skipped: [{drive, reason}]}` ([Search](#search)) |

A drive is `{id, name, storage, repository_id, store_id, created_by, created_at}`; `repository_id`
is `null` except for a repository drive, and `store_id` except for a store drive. An entry is `{id, drive_id, parent_id, kind, name, path, created_by,
created_at, modified_by, modified_at}`, `parent_id` `null` at the drive's top, plus for a file
`{size, content_type, sha256, state}` and for a shortcut `{target, status, reason?}`.

### Drives

- A drive's name is 1 to 255 bytes with no `/`, `\` or control character, not `.`, `..` or
  `.lake-folder`, and unique in the workspace (`conflict` at `/name`).
- `create_drive` with `storage: repository` names an existing repository of the workspace
  (`repository_id`; the caller must be able to read it, through `get_repository`) or a new one to
  create (`repository_name`, through `create_repository`, as the caller). A new repository has no
  `main` until its first change, and every drive_* capability resolves `main` first, so a bare
  repository's `drive_browse` answers `not_found`. `create_drive` therefore writes the
  repository's first change at once: the top folder's empty-folder marker `.lake-folder`
  (`commit_repository_files` takes an empty folder path for it), which `drive_browse` never lists.
- `delete_drive` needs `confirm_name` equal to the drive's name (`invalid_input` at
  `/confirm_name`). A `files` drive's entries and their objects are deleted first, children before
  parents; a `virtual` drive's entries are deleted; a `repository` drive's repository and a `store`
  drive's store are never touched, and the answer names them.

### Folders, Files and Shortcuts

The entry capabilities refuse a repository drive (`invalid_input` at `/drive_id`); a virtual
drive refuses uploads.

- Names follow the drive name rule and are unique in their folder, an unfinished upload's too. A
  name taken answers `conflict`, except for uploads: `on_conflict` is `rename` (default: the first
  free `name (2).ext`), `replace` (the upload replaces that ready file when it finishes: the old
  record and object are deleted then, if the old file is still the one of that name there) or
  `refuse`.
- `create_drive_folder_entry` makes each level of `path` that is missing (as `mkdir -p`) and
  answers the last; a level that is a file or shortcut is `conflict`.
- `list_drive_entries` lists a folder's ready entries by `folder_id` (or a slash `path` from the
  top; neither is the top), with the folders from the top down to it in `ancestors`, and the
  folder's unfinished uploads apart in `uploads`, each an entry with `age_seconds` and `expired`
  ([below](#unfinished-uploads-expire-lake588)).
- `move_drive_entry` changes `parent_id` (empty: the top) and/or `name` with a write conditional
  on the revision read, retried on a conflict. A folder never moves into itself. Only the record
  changes: a file's object is never copied.
- `delete_drive_entry` deletes the entry and, for a folder, everything in it, deepest first, each
  file's object before its record.
- Two writers creating the same name in one folder at once can both succeed: names are checked
  against one read of the drive, not claimed atomically.

### Shortcuts

`target` is `{kind, ...}`:

| `kind` | Fields | Checked |
|---|---|---|
| `repository` | `repository_id` | exists, not deleted, and the caller may read it |
| `repository_path` | `repository_id`, `path`, `ref?` (default `main`) | as above, and `path` is in the tree at `ref` |
| `drive_entry` | `drive_id`, `entry_id` | the drive and a ready entry exist in this workspace |
| `transcript` | `id` | the transcript exists in this workspace |
| `dataset`, `agent`, `model` | `id` | not checked |
| `project` | `name` | not checked |
| `url` | `url` (`http://` or `https://`, no whitespace, `"`, `<` or `>`) | not checked |

`create_drive_shortcut` refuses a checked target that is not there (`not_found` at `/target`).
Every listing and `get_drive_entry` checks again and answers `status`: `ok`, `broken` (with
`reason`) or `unknown` (a kind lake does not check, or a check that could not complete). A broken
shortcut never fails its folder's listing. Repository trees are read once per repository and ref
per call.

### Uploads

A file is uploaded in three steps, or in one call when small:

1. `begin_drive_upload` writes an `uploading` entry at `path` relative to `parent_id` (its folders
   made where missing), with a new opaque object key, and answers `block_size` (1 MiB) and
   `max_chunk_bytes` (8 MiB). `size` is the file's length, at most 64 GiB; `content_type` defaults
   from the name's extension. The calls that follow take the answer's `entry.id` as `id`. An uploading entry is not
   one of the folder's `items`; it is listed under `uploads`, and its name is taken until it
   finishes, is deleted or expires.
2. `append_drive_upload` sends bytes from `offset`, which is a multiple of `block_size`; every
   chunk but the last is a whole number of blocks, and no chunk runs past `size`. Chunks go in
   order. A chunk sent again (an answer lost) is accepted unchanged; one that does not continue
   the object is `conflict` at `/offset`. The answer's `received` is how many bytes the object
   holds now (read from its stored size, lake#588), not where this chunk ended, and `complete`
   says whether that is all of `size`: a chunk resent after the last one answers `complete:
   true`, so a client that trusts it finishes rather than sending everything again.
3. `finish_drive_upload` reads the object back, block by block: it must hold exactly `size` bytes
   (else `conflict`, "the upload is not complete"), and their SHA-256 becomes the entry's `sha256`
   (`invalid_digest` at `/sha256` when one was sent and differs). The entry becomes `ready` and
   is listed. Finishing a finished upload answers it again.

`upload_drive_entries` does all three for each file (at most 16 MiB decoded per call), in order.
Its items take the same shape as `upload_drive_files`' (lake#587): `path` (relative, may name
folders) or `name` (one name in the folder), `content`, and `content_type?`.

### Unfinished Uploads Expire (lake#588)

An upload not finished within 24 hours of its start (`drive_upload_expiry_us`, measured from the
entry's `modified_us`, which is its start while it uploads) has expired:

- `append_drive_upload` and `finish_drive_upload` refuse it (`conflict` at `/id`, "this upload
  expired").
- The next `begin_drive_upload` or `upload_drive_entries` into its folder removes every expired
  upload there first, object before record, so the new file takes the name instead of
  `name (2).ext`.
- Until then `list_drive_entries` lists it under `uploads` with `expired: true`;
  `delete_drive_entry` removes it at any time.

Expiry is applied by the next write into the folder rather than a sweep, so an expired upload in a
folder nobody writes to stays listed (and its object stays) until someone does or deletes it. The
Drives page names a folder's unfinished uploads with their age and removes them on request.

`read_drive_file` answers up to 16 MiB of a ready file as base64. `load_drive_dataset`
([below](#a-drive-file-as-a-dataset-lake593)) loads one as a dataset. A browser downloads with
`GET /drive/files/download?customer_id=&workspace_id=&drive_id=&id=[&inline=1]`
([http.md](../serving/http.md)): the route calls `get_drive_entry` as the caller (so the read is
authorized and audited), then streams the plaintext 1 MiB at a time from the object, opening each
block as it goes; the file is never held whole. `inline=1` serves it `inline` only for images,
PDF, plain text, CSV, Markdown, audio and video; anything else is an attachment. Every download
carries `X-Content-Type-Options: nosniff` and `Content-Security-Policy: sandbox; default-src
'none'`, and its ETag is the SHA-256.

## Store Drives

**Implemented 2026-10-11 in the source tree (lake#656); not deployed.** A drive with `storage:
store` opens one of the workspace's stores ([storage.md](storage.md)) as a drive, so a bucket or a
filesystem is browsed with the same pages and capabilities as workspace files. "Store" is the
API's word; the Lake Console says *storage*.

- **Creating one** names `store_id`, a store of this workspace that is a bucket (`object`) or a
  filesystem, Ready, with its connection delivered to Lake. Nothing is copied: the drive's record
  (a `lake_drives` record in the workspace Zetta, like every drive's) only names the store, and
  the bytes stay in the store's own provider. A volume is `invalid_input` at `/store_id`: *a volume
  is shown, not browsed* (its contents belong to the service that mounts it); a store not Ready is
  `workspace_not_ready`; another workspace's is `not_found`.
- **Browsing** goes through `control::StoreContents` (`include/lake/control/store_contents.hpp`):
  list a folder, stat, read a range, write a whole file, make a folder, move, remove. A
  `StoreBrowser`, injected as `ServerOptions::store_browser`, opens a store's contents from its
  record and its connection (`store_connection`'s answer). **No provider is browsable yet:** the
  only store Lake offers is a cluster volume, and every bucket and filesystem pair waits on
  lake#196's later phases. Without a browser for it, `create_drive` answers `not_supported` naming
  the provider and [lake#664](https://github.com/oramasearch/lake/issues/664), which tracks a
  `StoreContents` implementation per provider. The tests run the whole kind over an in-memory one.
- **Items** are addressed by their path in the store: `id`, `folder_id`, `parent_id` and `path`
  are all paths (`Reports/2026/q3.csv`; the top is `null` or empty). Each name in a path is a valid
  drive name. An item has no `created_by` (empty), `created_at` and `modified_at` are the
  provider's modified time when it gives one, and a file's `sha256` is `null`.
- **What works:** `list_drive_entries`, `get_drive_entry`, `create_drive_folder_entry` (levels made
  where missing; a file on the way is `conflict`), `move_drive_entry` (into another folder and/or
  renamed; a folder never into itself; a taken name is `conflict`), `delete_drive_entry` (a folder
  with everything in it), `upload_drive_entries` (16 MiB a call; `on_conflict` as for workspace
  files), `read_drive_file`, `GET /drive/files/download` (streamed 1 MiB a read, ETag from the
  provider's version), `load_drive_dataset` and `search_drives` (every name in the store, walked
  from the top, at most 10,000 items per drive; a store that cannot be read is listed in
  `skipped`).
- **What does not:** shortcuts (`invalid_input`: they go in a workspace-files or virtual drive),
  chunked uploads (`begin_drive_upload` is `invalid_input` at `/drive_id`), history, sharing, and
  `recent_drive_entries` (workspace files only).
- **Deleting the drive** (`delete_drive`) removes its record only and answers `store_id`; the store
  and everything in it stay. Deleting an item deletes it from the store itself, which keeps no
  history Lake can restore.

## Two Families of Calls (lake#587)

A drive's files are reached through the calls of the storage that keeps them, because the two
storages are different things: a repository drive's files are a liquid repository's tree (every
write a change, history, conflicts, per-folder policy), and a workspace-files drive's are records
and objects with none of that.

| Need | Repository drive (`drive_*`, by `repository_id`, `ref`, `path`) | Workspace-files or virtual drive (`*_drive_entry`, by `drive_id`, `id`) |
|---|---|---|
| List | `drive_browse` | `list_drive_entries` |
| Folder | `create_drive_folder` (`path` + `name`) | `create_drive_folder_entry` (`path`, levels made) |
| Upload | `upload_drive_files` | `upload_drive_entries`, or begin/append/finish |
| Move | `move_drive_path` (`destination`) | `move_drive_entry` (`parent_id`, `name`) |
| Delete | `delete_drive_path` | `delete_drive_entry` |
| Read bytes | `download_drive_file` (`content_base64` up to 16 MiB, and `download_url`), `GET /drive/download` | `read_drive_file`, `GET /drive/files/download` |
| History, sharing, previews | `drive_history`, `share_drive_folder`, `drive_preview`, … | none |

What is shared: `create_drive`, `get_drive`, `list_drives`, `rename_drive`, `delete_drive` and
`load_drive_dataset` take a drive of any kind; both upload calls take one item shape; and both
download routes are in OpenAPI ([../serving/http.md](../serving/http.md)). A client holding a
repository drive reads its `repository_id` from `get_drive` once. Making the `drive_*` calls also
take `drive_id` was left out: they are the repository's own capabilities (shared with the
repository's Files tab, its sharing links and the liquid drive client), authorized by repository
path policy, and taking either key would give each two inputs for one thing. `list_drive_entries`
on a repository drive says which call to use.

## A Drive File as a Dataset (lake#593)

`load_drive_dataset` loads a CSV or JSONL file of a drive into the workspace's datasets. `id` is
the file: its entry id in a workspace-files drive, its path (on `main`) in a repository drive; a
virtual drive is refused at `/drive_id`. `format` (`csv` or `jsonl`) comes from the file's name
(`.csv`, `.jsonl`, `.ndjson`) when it is not sent, and anything else is `invalid_input` at
`/format` before a byte is read. The call reads the bytes as the caller with `read_drive_file` or
`download_drive_file` (so the drive's own authorization and audit apply), then calls the workspace's
`upload_dataset` through `workspace_act` with `table_name`, `format`, `if_exists?` and the text;
its errors come back at this call's fields (`/table_name`, and `/id` for the content). At most 16
MiB (`size_invalid` at `/id`), as `upload_dataset` takes. Nothing is stored anywhere but the
dataset. The Drives page offers it as **Load as dataset** on a CSV or JSONL file.

## A Drive File as a Model (lake#654)

`load_drive_model` brings a model file of a drive into the workspace as a standalone model, the same
way: `id` is the file, a virtual drive is refused at `/drive_id`, the bytes are read as the caller,
and they go, as `weights_base64` so a binary ONNX or UBJSON file arrives intact (lake#681), to the
workspace engine's `register_model` through `workspace_act` with `name` (or `version_of`),
`format?`, `feature_names?`, `kind?`, `metrics?` and `source` (`drive <drive name>: <file name>`).
The file may be any format the engine converts ([models.md](models.md#bring-a-model-in-a-common-format-and-download-its-weights-lake681)). A new model
needs `name` (`invalid_input` at `/name` before a byte is read). The engine's refusal of the file
(`not_supported` for a format it cannot read) comes back at `/id`. At most 16 MiB. Train → Models'
**New model**, Bring one, A drive offers it ([models.md](models.md#a-standalone-model-lake654)).

## Search

`search_drives` finds the drives, folders, files and shortcuts of the workspace whose **name**
contains `q`, ignoring the case of A to Z (other characters compare exactly; a substring is
enough). With `drive_id` it searches that drive only (`not_found` at `/drive_id` for a drive this
workspace does not hold). It never looks inside a file's bytes.

| Drive | What is searched | Read |
|---|---|---|
| any | the drive's own name | `list_drives`, once |
| `files`, `virtual` | every ready folder, file and shortcut name (an unfinished upload is never found) | each drive's `lake_drive_entries`, once, as `list_drive_entries` reads them |
| `repository` | the name of every folder and file path in the repository's tree at `main` (the `.lake-folder` marker never) | the tree, once per drive, off the server thread, when the caller may read the repository |

A match is `{drive: {id, name, storage, repository_id}, kind, id, parent_id, name, path, folder}`:
`kind` is `drive`, `folder`, `file` or `shortcut`; `path` is the item's path from the drive's top
(empty for a drive) and `folder` the path of the folder holding it; `id` and `parent_id` are entry
ids in a workspace-files or virtual drive and `null` otherwise (a repository item is addressed by
its path). A file adds `size` and, outside repository drives, `content_type` and `modified_at`; a
shortcut adds `target` and its checked `status` (and `reason`), as `list_drive_entries` answers
them. Drives come first, then matches by drive name and path; the first `limit` are answered and
`truncated` says whether more matched. Only the shortcuts answered are checked.

**Repository drives, and why paths only.** A repository drive's files are a liquid repository, so
the honest bounded search is its tree at `main`: one read per drive, the same tree `drive_browse`
lists, giving each path's name and size. A file's modified time is not in the tree (`drive_browse`
computes it by walking the history), so a repository match has none. Searching the *contents* of a
repository drive's files is lake's code search (`search` with `repository`,
[search.md](search.md)), which the Search page offers as its Code place. A repository drive whose
tree cannot be read (no repository store, the repository deleted, the caller not allowed to read
it, a read failure) is listed in `skipped` with the reason and never fails the call. Read access is
the repository's own: a person who sees a repository folder only through a shared link finds
nothing of it here.

Like `list_drive_entries`, the read authorizes `read` on the account before it reads, and its audit
event (in the workspace's log) records the `drive_id` searched and the number of matches, never the
query.

## How File Bytes Are Kept

Each file's bytes are one object in the workspace Zetta's `lake_files` bucket (created on first
use, without a quota), under the key `f/<uuid>`, which says nothing about the file. The record
holds the key; a move or rename never touches the object.

- **Sealed in blocks.** The plaintext is cut into 1 MiB blocks (the last shorter). Block `n` is
  stored at object offset `n × (1 MiB + 28)` as its 12-byte nonce, the AES-256-GCM ciphertext and
  the 16-byte tag, so any range is read by reading and opening only its blocks.
- **Keys.** Two keys per workspace are derived with HKDF from `LAKE_INSTANCE_KEK`
  (`lake/drive-files/v1/seal/<workspace>` and `lake/drive-files/v1/nonce/<workspace>`), as
  workspace records derive theirs. Not the workspace store's data key: that key is kept inside
  `lake_store` and opens only under that store's lease, which file requests do not hold.
- **Nonces.** A block's nonce is the first 12 bytes of HMAC-SHA-256 under the nonce key of the
  object key, the block index and the plaintext (a synthetic nonce). The same block always seals
  to the same bytes, so a retried chunk is an exact repeat, which Zetta's exact-offset append
  accepts as unchanged; two different blocks never share a nonce.
- **Bound to their place.** The associated data is `lake-drive-files/v1|<workspace>|<key>|<index>`:
  a block copied to another index, object or workspace does not open (`corrupt`).
- **Durable.** Every append is `?durable=true`: Zetta fsyncs it before it answers, so an
  acknowledged chunk survives a crash, and the upload resumes at the next chunk.
- **No lease.** The bucket has no owner lease, unlike `lake_store`. Each object has one writer
  (the upload that made its key; the entry is not `ready` until finished), every append names its
  exact offset, and an object is never rewritten in place, so two replicas serving the same upload
  can only append identical bytes. The store owner's lease protects liquid's in-memory directory
  of `lake_store`, which `lake_files` does not have.
- **Removal.** Deleting a file deletes its object (durably) before its record, so a failure leaves
  a record whose object is gone, never an object no record names. An abandoned upload's object
  stays until its entry is deleted, or until it has expired and another upload of its name comes
  into its folder.

**Known gap:** `lake_files` is created without a quota, and lake's storage quota and usage
figures read the `lake_store` bucket, so workspace files are not counted against the account's
storage quota yet. The workspace Zetta's own volume is the bound.

## Errors

| Code | When |
|---|---|
| `invalid_input` | a name, path, media type, offset or target is invalid; a repository drive given to an entry capability; an upload to a virtual drive; `confirm_name` does not match |
| `not_found` | no such drive, folder, entry or upload in this workspace; a checked shortcut target is gone |
| `conflict` | a name taken; a folder level that is not a folder; a chunk out of order; an upload not complete; a finished upload sent more bytes; an expired upload |
| `invalid_digest` | `finish_drive_upload`'s `sha256` differs from the bytes |
| `size_invalid` | a chunk over 8 MiB, an inline upload or read over 16 MiB, a file over 64 GiB |
| `not_supported` | this lake has no workspace Zetta object transport (no workspace files); a store drive's store is of a provider Lake cannot browse yet ([lake#664](https://github.com/oramasearch/lake/issues/664)) |
| `workspace_not_ready` | a store drive's store is not Ready, or its connection has not reached Lake |
| `corrupt` | a stored block does not open with this workspace's key |
| `quota_exceeded`, `store_unavailable` | the workspace Zetta refused or did not answer |

## Verify

- `tests/test_drives.cpp`: the folder tree and names; sealing (deterministic, bound to key, index
  and workspace); chunked writes, retries, a lost answer, out-of-order and short objects, any-range
  reads; a workspace-files drive end to end (folders from an upload path, chunked upload, finish,
  moves without copying, rename/replace/refuse, recursive delete, recent files, drive delete);
  another account refused; a virtual drive's shortcuts going broken; the audit routing; and over
  HTTP, a repository drive's `main` from creation (a bare repository's `drive_browse` is
  `not_found`) and a streamed download of a 3 MiB file.
- `Drives.SearchFindsDrivesFoldersFilesAndShortcutsByName`: matches across a workspace-files and a
  virtual drive in either case, with their paths, sizes and shortcut status; an unfinished upload
  never found; a limit with `truncated`; one drive; another workspace; another account refused.
  `DrivesHttp.SearchFindsTheNamesOfARepositoryDrivesPaths`: a repository drive's folder and file
  paths, without the folder marker.
- `SchemaConformance.EveryToolAndRequiredOutputUsesHttpContract` calls every capability above and
  validates its answer against its output schema.
- `Drives.AStoreDriveBrowsesItsStoreInPlaceAndLeavesItWhenDeleted` (lake#656): a store drive over
  an in-memory `StoreContents`, created with no copy; its folders and files listed by path; an
  upload (renamed, refused), a folder, a read, a move and a rename made in the store; search by
  name; an item deleted; the drive deleted with the store and its contents still there.
  `Drives.AVolumeIsShownNotBrowsedAndAStoreLakeCannotBrowseIsRefused`: a volume refused, a bucket
  `not_supported` without a browser (naming lake#664), a store not Ready, a store that is not
  there.
- `Drives.AResentChunkAnswersTheUploadAndAbandonedUploadsExpire` (lake#588): a resent chunk's
  true `received` and `complete`, unfinished uploads under `uploads`, an expired one refusing
  chunks and giving up its name, and `upload_drive_entries` taking `name`.
- `DrivesHttp.ARepositoryDriveWriteIsSearchedDownloadedAndLoaded` (lake#584, lake#587, lake#593):
  a drive write found by hot search, then moved and deleted; `download_drive_file`'s bytes, media
  type and `download_url`, whose route serves `text/csv` with the file's name; both download routes
  in OpenAPI; and `load_drive_dataset` sending the CSV to `upload_dataset`; `load_drive_model`
  sending a model file's bytes in base64, its name, metrics, format, column names and source to
  `register_model`, refusing a new
  model with no name at `/name`, and the engine's refusal of the file at `/id` (lake#654).
  `DrivesHttp.AWorkspaceFileDownloadStreamsItsBytes` checks `Content-Disposition` and loads a JSONL
  workspace file.

## Change It

- A new shortcut kind: `drive_shortcut_problem` (`src/control/drives.cpp`), `target_schema` and
  `ShortcutChecker` (`src/api/drives_capabilities.cpp`), and `frontend/src/pages/drives/targets.ts`.
- A new storage: `known_drive_storage`, `create_drive`, and a `DriveStorage` implementation in
  `frontend/src/pages/drives/storage.ts`.
- A provider a store drive can browse: a `control::StoreContents` implementation and a
  `StoreBrowser` that opens it for that type and provider, set in `ServerOptions::store_browser`
  ([lake#664](https://github.com/oramasearch/lake/issues/664)).
- The block format is versioned by its labels (`v1`); a new format gets new labels and a record
  field naming it, and keeps reading `v1`.
