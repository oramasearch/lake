# Drives Pages

**Status: implemented 2026-10-10 in the source tree (lake#561); not deployed.** Data → Drives is a
workspace's file manager: its drives, each keeping its files in a repository, as workspace files,
or as shortcuts only ([../api/drives.md](../api/drives.md)). The pages are workspace pages
(`/w/<id>/…`) in `frontend/src/pages/drives/`. The customer guide is
[`docs/guide/drives.md`](../guide/drives.md).

## Pages

| Path | File | What it shows | Capabilities |
|---|---|---|---|
| `/drives` | `DrivesPage.tsx`, `NewDrive.tsx` | four figures (drives, and each kind), a filter and kind chips, a card per drive, **Recent Files**; **New drive** opens a drawer | `list_drives`, `recent_drive_entries`, `create_drive`, `list_repositories` |
| `/drives/drive?id=&folder=[&focus=]` | `DrivePage.tsx`, `FolderPicker.tsx`, `ShortcutDialog.tsx` | one drive as a file manager; `folder` is the folder's key (a path in a repository drive, an entry id otherwise); `focus` (a shortcut to an item) opens the folder that holds it and selects it | `get_drive`, then the drive's storage (below), `get_drive_entry`, `rename_drive`, `delete_drive` |

The rail's **Drives** item sits under Data after Repositories and owns `/drives/drive`.

## One Interface over Three Storages

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
  reason and no link.
- **Load as dataset** (lake#593). A CSV or JSONL file's menu (`datasetFormat` in `paths.ts`)
  opens `LoadDataset`: a dataset name started from the file's name (`datasetName`) and the
  take-the-next-free-name box; it calls `load_drive_dataset` with the drive and the item's key and
  opens **Data → Datasets** with the loaded banner, as an upload there does. Both repository and
  workspace-files drives offer it.
- **Unfinished uploads** (lake#588). A workspace-files folder whose listing has `uploads` shows a
  quiet **Unfinished Uploads** banner naming each with its age (or "expired"); **Remove** deletes
  them with `delete_drive_entry`. They hold their names until removed or, after 24 hours, until
  the next upload of the name.
- **Deletes** name their consequence: a repository keeps history; workspace files have none; a
  shortcut's target stays. **Drive settings** renames the drive and deletes it once its name is
  typed.

## Verify

`frontend/tests/drives.test.ts` covers paths, names, sorting, range selection, the upload and move
plans, upload chunks, what each shortcut opens, and which files load as datasets under which name. `npx tsc --noEmit -p .`, `npm test` and
`npm run build` from `frontend/`.
