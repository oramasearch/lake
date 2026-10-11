# Drives Pages

**Status: implemented 2026-10-10 in the source tree (lake#561); not deployed.** Data → Drives is a
workspace's file manager: its drives, each keeping its files in a repository, as workspace files,
as shortcuts only, or in one of its buckets or filesystems (a storage drive, lake#656)
([../api/drives.md](../api/drives.md)). Its **Storage** tab holds the workspace's buckets,
filesystems and volumes, moved from Datasets (lake#656, [below](#storage)). The pages are workspace pages
(`/w/<id>/…`) in `frontend/src/pages/drives/`. The customer guide is
[`docs/guide/drives.md`](../guide/drives.md).

## Pages

| Path | File | What it shows | Capabilities |
|---|---|---|---|
| `/drives` | `DrivesPage.tsx`, `NewDrive.tsx` | tabs **Drives** and **Storage** (`?tab=storage`). Drives: four figures (drives, and three kinds), a filter and kind chips, a card per drive, **Recent Files**; **New drive** opens a drawer (a storage drive picks one of the workspace's buckets or filesystems, volumes listed disabled) | `list_drives`, `recent_drive_entries`, `create_drive`, `list_repositories`, `list_stores` |
| `/drives?tab=storage` | `StorageTab.tsx`, `stores.ts` | the workspace's storage ([below](#storage)) | `list_stores`, `list_store_providers`, `create_store`, `delete_store`, `store_connection`, `usage_over_time` (`volumes`), `list_drives` |
| `/storage` | `StorageRedirect.tsx` | Go to's **Storage**: sends the person to `/drives?tab=storage` | — |
| `/drives/drive?id=&folder=[&focus=]` | `DrivePage.tsx`, `FolderPicker.tsx`, `ShortcutDialog.tsx`, `search.ts`, `MatchLink.tsx` | one drive as a file manager; `folder` is the folder's key (a path in a repository drive, an entry id otherwise); `focus` (a shortcut to an item) opens the folder that holds it and selects it | `get_drive`, then the drive's storage (below), `get_drive_entry`, `search_drives`, `rename_drive`, `delete_drive` |

The rail's **Drives** item sits under Data after Repositories and owns `/drives/drive` and
`/storage`. Go to (⌘K) and the top bar's search list **Storage** as its own entry.

## One Interface over Four Storages

The file manager never asks which kind of drive it shows. `storage.ts` defines `DriveStorage`:
`list(folder)`, `createFolder`, `upload(folder, items, progress)`, `rename`, `move(items, folder)`,
`remove`, `createShortcut`, `downloadHref`, `viewHref`, and `can` (`upload`, `shortcuts`,
`history`, `share`), which decides the buttons. `storageFor(drive, scope)` picks the
implementation:

| Implementation | Item key | Calls |
|---|---|---|
| `RepositoryStorage` | path | `drive_browse`, `create_drive_folder`, `upload_drive_files`, `move_drive_path`, `delete_drive_path` on `main`, unscoped; files open in `/drive/preview` and download from `/drive/download` (the `download_url` `download_drive_file` answers) |
| `FilesStorage` | entry id | `list_drive_entries`, `create_drive_folder_entry`, `upload_drive_entries` (files up to 4 MiB, several per call, at most 12 MiB a call), `begin_drive_upload` / `append_drive_upload` (8 MiB pieces at block boundaries) / `finish_drive_upload` (larger files), `move_drive_entry`, `delete_drive_entry`, `create_drive_shortcut`; files download from `/drive/files/download`, and open in the browser with `inline=1` for the media types it shows safely (`INLINE_TYPES`) |
| `VirtualStorage` | entry id | as `FilesStorage`, without uploads |
| `StoreStorage` | path in the store | as `FilesStorage` (lake answers a store drive's items with their path as `id`), with every upload through `upload_drive_entries` (several files a call, at most 12 MiB a call) and each file at most 16 MiB (a larger one is listed in the warning banner); no shortcuts |

A repository drive's upload makes its folders first (one change each; a folder already there is
fine) and then sends each folder's files in as few changes as the 64 MiB body allows
(`planUpload`, `chunkBySize`); a file too large for one request is listed in the warning banner.
A workspace-files upload sends each file at its relative path and lake makes the folders.

## Behaviour

- **Uploads.** Upload (files), Upload a folder (`webkitdirectory`: each file keeps its path in the
  folder) and dropping files or folders on the list (`webkitGetAsEntry`, folders walked) all go
  through one uploader with a progress banner; a drop on a folder row uploads into that folder.
- **Selection.** A row click selects it; Shift-click selects the range from the last click
  (`rangeSelect`), Ctrl/⌘-click toggles; the header box selects every shown row. The selection
  bar moves or deletes the selected items.
- **Moves.** Dragging carries the selected keys under `application/x-lake-drive-items`, so a drag
  from the desktop is never a move. A folder row and every crumb of the path take drops. **Move**
  opens `FolderPicker`, which browses the drive's folders (the folders being moved cannot be
  opened or chosen).
- **Shortcuts.** `ShortcutDialog` chooses repositories (this workspace's), transcripts and drive
  items (a drive, then an item through `FolderPicker` in item mode) from lists, and takes a
  dataset, project, agent or model by name and a web address typed. `targets.ts` checks the target
  as lake does (`targetProblem`), sends only its kind's fields (`cleanTarget`), and maps each
  shortcut to the page it opens (`targetHref`); a broken shortcut shows a **Broken** chip with the
  reason and no link. Choosing a target suggests the shortcut's name; focusing **Name** selects the
  suggestion, so typing replaces it rather than appending to it, and once the person has typed a
  name a new target leaves it alone (`nameAfterTarget`, `selectsOnFocus`, lake#580).
- **Load as dataset** (lake#593). A CSV or JSONL file's menu (`datasetFormat` in `paths.ts`)
  opens `LoadDataset`: a dataset name started from the file's name (`datasetName`) and the
  take-the-next-free-name box; it calls `load_drive_dataset` with the drive and the item's key and
  opens **Data → Datasets** with the loaded banner, as an upload there does. Both repository and
  workspace-files drives offer it.
- **Unfinished uploads** (lake#588). A workspace-files folder whose listing has `uploads` shows a
  quiet **Unfinished Uploads** banner naming each with its age (or "expired"); **Remove** deletes
  them with `delete_drive_entry`. They hold their names until removed or, after 24 hours, until
  the next upload of the name.
- **Search.** The toolbar's box (*Search this drive…*) filters the open folder's items as each
  letter is typed, the instant first step. Once typing settles (250 ms) it also calls
  `search_drives {drive_id, q}` and lists, under the folder, **Elsewhere in This Drive**: every
  match outside the open folder with its icon, its name with the match marked, and a muted line
  with the drive and folder path (which opens that folder), plus a file's size and time. A folder
  match opens in place; a file and a shortcut open as their row would. A repository drive is
  searched by its paths at `main`; one that cannot be read says why.
- **Deletes** name their consequence: a repository keeps history; workspace files have none; a
  shortcut's target stays. **Drive settings** renames the drive and deletes it once its name is
  typed.

## Storage

The Storage tab (lake#656) is what was the Storage card on Data → Datasets, which keeps datasets
only now. It shows whether or not the workspace is ready (storage does not need it).

- **Figures:** Storage (how many, or how many are changing), Opens as a drive (Ready buckets and
  filesystems), Volumes (shown, not browsed) and Size (the GiB asked for, in all).
- **The table:** each store's name with its type and provider in words (*Volume · cluster disk*),
  its size, **This Month** (its `volumes` time from `usage_over_time` by object, linking to
  Usage), the status (Waiting for the operator, Creating, Connecting, Ready, Deleting, Failed) with
  the operator's message, the endpoint, and the actions: **Open as a drive** (a Ready bucket or
  filesystem, opening New drive with it chosen) or **Open drive** when a drive already opens it;
  a *Shown, not browsed* chip on a volume; **Show connection** once Ready; **Delete**.
- **New storage** and **Delete** (the name typed) are the dialogs Datasets had; the list checks
  again every 3 seconds while a store is changing.
- **Words.** A *drive* is where people work with files; *storage* is a bucket, filesystem or volume
  that holds bytes. The page says storage; the API's name is *store* (`list_stores`).
- **Not shown yet:** which services and apps mount a store, a volume's link to that service and
  Ship → Services' link back, and each store's used space. Lake records none of them
  ([lake#666](https://github.com/oramasearch/lake/issues/666)).

Old links: `/train/data#storage` goes to `/drives?tab=storage` (`movedFromDatasets`), and
`/drives#storage` opens the tab too.

## In Data → Search

Search's **Drives** place (`frontend/src/pages/search/SearchPage.tsx`, on by default with the
others, `?in=…,drives,…`) calls `search_drives` in the workspace chosen in the top bar. Each row
shows the kind's icon (`matchIcon`: drive, folder, file, shortcut, red when broken), the name with
the match marked (`components/Highlight.tsx`, as the Docs reader marks one) and a muted line with
the drive and folder path (`matchWhere`). `matchLink` (`search.ts`) decides where a row goes: a
drive or folder opens `/drives/drive?id=&folder=` (a folder's key is its path in a repository or
storage drive, its id otherwise); a file opens the repository preview (`/drive/preview`), or a workspace
file in the browser with `inline=1` when it is one of `INLINE_TYPES` and downloads otherwise; a
shortcut opens its target (`targetHref`), or, when broken, the folder that holds it with the
shortcut selected (`focus`).

## Verify

`frontend/tests/drives.test.ts` covers paths, names, sorting, range selection, the upload and move
plans, upload chunks, what each shortcut opens, which files load as datasets under which name,
where each search match opens (`matchLink`, `matchWhere`, `folderKey`, `parentKey`), and the
Storage tab (`stores.ts`: each store's state, the figures, which open as drives, a volume never),
the storage drive kind, the old `#storage` link, and Datasets without storage or connections. `npx tsc --noEmit -p .`, `npm test` and
`npm run build` from `frontend/`.
