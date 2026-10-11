# Drive pages

**Status: implemented for lake #8, #43, #116 and #123; in the web app since lake#561.** The drive is
a repository view for people who work with folders and files. It uses the same liquid paths,
stable node identifiers, blobs, changes, conflicts, policy checks and audit records as every API
caller. Its pages are in `frontend/src/pages/repositories/` and need a signed-in viewer; a folder
opened through a share link is in [sharing.md](sharing.md).

## Browse and change

`/drive?repository_id=<id>&ref=<ref>&path=<path>[&view=list|grid&sort=name|size|modified&direction=asc|desc]`
(`DrivePage.tsx`, `drive_browse`) is a file manager: the path bar, the folder's figures (folders,
files, their size), a toolbar, a drop zone and the folder's children. The toolbar filters by name in
the page and chooses the sort (name, size or last structural change), its direction and list or
grid; those three are written to the URL at once. List view shows kind, size and modified columns;
grid view shows the same values on cards. Each item's menu holds History, Download (files), Rename,
Move and Delete; the header holds Share, New folder, Upload and a menu with History and Conflicts.

| Control | Capability | History behaviour |
|---|---|---|
| New folder | `create_drive_folder` | commits a folder marker as a new change |
| Upload (choose or drop files) | `upload_drive_files` | commits every chosen file in one change |
| Rename | `move_drive_path` | moves the stable node in a new change |
| Move | `move_drive_path` | moves the stable node in a new change |
| Delete, after a confirmation | `delete_drive_path` | removes the current path in a new change |

Upload sends the files as base64 inside one JSON body, so the page refuses a selection larger than
the REST body limit (64 MiB, encoding included) before sending it. Choosing files (Upload, or pressing the drop
zone) or dropping them on the drop zone uploads them at once. New folder, Rename and Move are
dialogs; Delete is a confirmation dialog. Before lake#561 upload was a multipart form posted to `/drive/upload`, with
drag and drop added by a script. Empty folders use a reserved `.lake-folder` marker hidden from
drive results. Deleting or moving a current path never removes its prior versions from liquid
history; the delete confirmation says so.

## Preview and download

`/drive/preview` (`DrivePreviewPage.tsx`) reads `drive_preview`, which chooses a renderer from
`DriveFormatRegistry` and answers escaped markup that the page inserts as it is: Markdown rendered
on the server, text, CSV and JSON; an image element; PDF in the browser's own viewer; video and
audio with native controls. An unregistered format shows its size and digest and offers the
download instead of guessing. **Download** is lake's `/drive/download` route, which answers the
exact stored bytes as an attachment.

The extension table is deliberately local. When liquid #29's format registry lands, its codec
metadata plugs into `DriveFormatRegistry::defaults()` as the renderer selection source; the page
and capability contracts do not change.

## History and conflicts

`/drive/history` (`DriveHistoryPage.tsx`, `drive_history`) lists only changes whose structural diff
touches the selected old or new path. Each available version can be restored
(`restore_drive_version`), which writes a new change. Selecting two text versions opens
`/drive/history/diff` (`DriveDiffPage.tsx`, `diff_drive_versions`), which reads both immutable
revisions and shows their line difference.

Each row is the path's state after its change (lake#123). Uploading a file over an existing one is
an ordinary overwrite: the upload removes the old node and adds a new one at the same path, so its
change carries two structural entries for the path. The row takes the entry that leaves a file at
the path, names the blob it replaced, and reads **modified**; the new version is current and the
previous one stays a normal prior version that can be compared and restored. A restore writes the
same node and also reads **modified**. Only a change that leaves no file at the path is shown as
**Deleted**, without compare or restore controls.

The repository conflicts page (`/conflicts`) links each path to `/drive/conflict`
(`DriveConflictPage.tsx`, `drive_conflict`). Both pages state the conflict kind in person-facing
words, identify the exact path plus byte or stable-node region, show Liquid's explanation and
suspicious-merge reasons, and keep the base/left/right and optional proposal identities as
evidence. The drive page also identifies each preserved file by digest and size and shows a text
difference when both values have a text renderer; other formats keep the two-value presentation
without claiming a textual comparison.

A signed-in writer can keep the left value, keep the right value, accept a reviewed proposal when
one exists, or enter prepared replacement text (`resolve_conflict`, which checks the path against
the conflict region and returns the `conflict_resolution` value and its change). A resolution
returns to the containing folder. The history table lists each conflict identity carried by a
conflict-bearing change.

## What the scenarios prove

The Postgres-backed scenario suite starts an in-process Lake and exercises the public HTTP and
Liquid client contracts:

- `DriveScenario.UserSyncsSharesAndSeesHistoryEndToEnd` proves that a regular user can sign up,
  synchronize a drive folder, share it with another regular user, and let that recipient browse the
  folder through the API and read its history.
- `DriveScenario.ConcurrentOfflineEditsExposeAndResolveFirstClassConflict` proves that clients
  editing the same file while disconnected retain both edits under distinct names, publish both
  paths, expose the complete structural conflict and its preserved values through the API,
  and resolve it through the public action. It requires the bootstrap actor identity fix from
  [liquid #262](https://github.com/oramasearch/liquid/pull/262). The scenario proof uses the recorded
  Liquid `main` pin, including first-class conflict records and structural convergence.
- `DriveScenario.ReplacingAnUploadedFileKeepsThePreviousVersionRestorable` proves that two uploads
  of the same file name leave two available versions on the API, that the two
  compare and the previous one restores, and that a real deletion stays distinct (lake#123). The
  same exercise runs on the memory store's account store and on a switched workspace store
  (`WorkspaceStoreDrive.*`).
- `DriveScenario.SyncedFolderPullsAsRepositoryAndBuildsProject` proves that a drive folder is the
  same Liquid repository: the public repository client pulls and materializes it, after which CMake
  builds and runs the contained project.

The last case exercises the repository create, pull, and materialize APIs directly. The same
drive-client contract powers the `liquid clone` command implemented by
[liquid #259](https://github.com/oramasearch/liquid/pull/259).

## Verify and change

In a browser, signed in as a writer: create a folder, upload two files by choosing and by dropping,
upload one of them again and check that History shows **modified** with a comparable, restorable
previous version, rename, move and delete (the history keeps the old versions), preview a Markdown,
a CSV, an image and an unknown file, and download one and compare its bytes. The capability
contracts are in [../api/drive.md](../api/drive.md); the scenarios above run against lake's API.

A new drive action is a capability first, then a control here. A new preview format is a
`DriveFormatRegistry` renderer on the server; the page inserts what `drive_preview` answers.

## Linux client states

Linux desktop presentation remains a liquid client concern. Lake documents the shared states:

| State | Hydration and offline behavior | Badge meaning |
|---|---|---|
| Online-only | Metadata is local; file bytes hydrate on open and require connectivity | Cloud outline |
| Pinned | The full selected subtree is hydrated and retained for offline reads | Filled pin/check |
| Locally modified | Local bytes remain available and await reconciliation; conflicts retain both values | Sync or conflict badge |

Pin changes control hydration policy, never repository retention. Offline writes are local changes
and publish through the same three-tree reconciler when connectivity returns. GNOME and KDE badge
and pin behavior is tracked by [liquid #196](https://github.com/oramasearch/liquid/issues/196).
