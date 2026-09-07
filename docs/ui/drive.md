# Drive pages

**Status: implemented for lake #8 and #43.** The drive is a server-rendered repository view for people
who work with folders and files. It uses the same liquid paths, stable node identifiers, blobs,
changes, conflicts, policy checks, and audit records as every API caller.

## Browse and mutate

`GET /drive?repository_id=<id>&ref=<ref>&path=<path>` provides breadcrumbs and immediate children.
The display control selects list or grid and sorts folders and files by name, size, or last
structural change. List view exposes size and modified columns; grid view carries the same values
on cards.

Signed-in users receive these capability-backed controls:

| Control | Route | Capability | History behavior |
|---|---|---|---|
| New folder | `POST /drive/folder` | `create_drive_folder` | Commits a folder marker as a new change |
| Upload | `POST /drive/upload` | `upload_drive_files` | Commits all selected files in one change |
| Rename | `POST /drive/rename` | `move_drive_path` | Moves the stable node in a new change |
| Move | `POST /drive/move` | `move_drive_path` | Moves the stable node in a new change |
| Delete | `POST /drive/delete` | `delete_drive_path` | Removes the current path in a new change |

Upload is an ordinary multipart form. A small progressive-enhancement island adds drag and drop,
submits through that form, and contains no authorization or repository logic. Empty folders use a
reserved `.lake-folder` marker hidden from drive results. Deleting or moving a current path never
removes its prior versions from liquid history.

## Preview and download

`GET /drive/preview` chooses a renderer from `DriveFormatRegistry`. Markdown is rendered and HTML
escaped on the server. Text, CSV, and JSON remain server-rendered; images use an image element;
PDF uses the browser's native document object; video and audio use native media controls. An
unregistered format shows its size and digest and offers `GET /drive/download` instead of guessing.

The current extension table is deliberately local. When liquid #29's format registry lands, its
codec metadata plugs into `DriveFormatRegistry::defaults()` as the renderer selection source. The
page and capability contracts do not change.

## History and conflicts

`GET /drive/history` lists only changes whose structural diff touches the selected old or new
path. Each available version can be restored with `POST /drive/history/restore`; restore creates a
new change. Selecting two text versions opens `GET /drive/history/diff`, which reads both immutable
revisions and renders their structural line difference.

The repository conflicts page links each path to `GET /drive/conflict`. That page states that both
values are preserved, identifies each by digest and size, and shows a structural text difference
when both values have a text renderer. Other formats retain the two-value presentation without
claiming a textual comparison.

The history table also lists every immutable digest from a conflict-bearing change. A user can
therefore confirm both retained values without leaving the path history page.

## What the scenario proves

The Postgres-backed scenario suite starts an in-process Lake and exercises the public HTTP and
Liquid client contracts:

- `DriveScenario.UserSyncsSharesAndSeesHistoryEndToEnd` proves that a regular user can sign up,
  synchronize a drive folder, share it with another regular user, and let that recipient browse the
  folder through the API and read its rendered history.
- `DriveScenario.ConcurrentOfflineEditsKeepBothNamedVersionsInHistory` proves that clients editing
  the same file while disconnected retain both edits under distinct names, publish both paths, and
  expose both immutable values through the history API and page. It requires the bootstrap actor
  identity fix from [liquid #262](https://github.com/oramasearch/liquid/pull/262). The scenario
  proof uses Liquid `11fdf44`, including the chunk-index compaction fix from
  [liquid #274](https://github.com/oramasearch/liquid/pull/274).
- `DriveScenario.SyncedFolderPullsAsRepositoryAndBuildsProject` proves that a drive folder is the
  same Liquid repository: the public repository client pulls and materializes it, after which CMake
  builds and runs the contained project.

The last case exercises the repository create, pull, and materialize APIs directly. The same
drive-client contract powers the `liquid clone` command implemented by
[liquid #259](https://github.com/oramasearch/liquid/pull/259).

## Rendering contract

All drive page functions in `include/lake/api/web_ui.hpp` are pure string renderers. Capability
tests cover repository reads, writes, policy routing, audit intent, version comparison, and
conflict bytes. String tests cover escaped Markdown, all built-in preview families, list/grid
controls, mutation forms, history selection, and conflict presentation. The seeded fixture takes
light and dark screenshots of folder list, folder grid, preview, history, text comparison,
conflict, sharing, and public link pages through the production HTTP routes.

Linux desktop presentation remains a liquid client concern. Lake documents the shared states:

| State | Hydration and offline behavior | Badge meaning |
|---|---|---|
| Online-only | Metadata is local; file bytes hydrate on open and require connectivity | Cloud outline |
| Pinned | The full selected subtree is hydrated and retained for offline reads | Filled pin/check |
| Locally modified | Local bytes remain available and await reconciliation; conflicts retain both values | Sync or conflict badge |

Pin changes control hydration policy, never repository retention. Offline writes are local changes
and publish through the same three-tree reconciler when connectivity returns. GNOME and KDE badge
and pin behavior is tracked by [liquid #196](https://github.com/oramasearch/liquid/issues/196).
