# Drive API

**Status: implemented for lake #8 and #43.** Drive is a repository view for people who work with
folders and files. It does not create a second store: paths, blobs, changes, conflicts, refs, and
deduplication remain liquid repository values.

## Capabilities

| Capability | Authorization | Result |
|---|---|---|
| `drive_browse` | `read` on the nearest path object, falling back to the repository | Immediate children, size, last structural change, and requested ordering |
| `drive_preview` | `read` on the path | A server-rendered preview selected by extension |
| `download_drive_file` | `read` on the path | Exact bytes with size, digest, and media type |
| `create_drive_folder` | `write` on the containing folder | A new liquid change containing an empty-folder marker |
| `upload_drive_files` | `write` on the containing folder | One liquid change containing all decoded uploads |
| `move_drive_path` | `write` on the source and destination parent | A new liquid move preserving the stable path node |
| `delete_drive_path` | `write` on the path | A new liquid deletion without history removal |
| `drive_history` | `read` on the path or nearest retained parent | Changes whose structural diff names `path` as its old or new path |
| `diff_drive_versions` | `read` on the path or nearest retained parent | The two immutable files and a structural text difference |
| `drive_conflict` | `read` on the path | Preserved conflict values and a text difference when registered |
| `restore_drive_version` | `write` on the path or nearest retained parent | A new liquid change containing the file bytes from `change_id` |

A path object has `object_type=path` and external id `<repository UUID>:<liquid node id>`. The
stable liquid tree node id means a move does not discard policy. Reads use the nearest existing
folder object, so repository policy remains the default. Move authorization checks both the
source and destination location. Every mutation calls `commit_repository_files`; the HTTP forms
do not write the store directly.

## Formats

The built-in registry covers plain source and configuration text, Markdown, PNG/JPEG/GIF/WebP,
CSV, JSON, PDF, MP4/WebM/Ogg video, and MP3/M4A/Ogg/WAV audio. Markdown and all tabular values are
escaped before becoming HTML. PDF and media bytes are supplied to browser-native elements.
Unknown extensions return the `download` presentation with `application/octet-stream`.

Liquid #29's format registry is not yet on main. `DriveFormatRegistry::defaults()` is the explicit
integration point: replace its extension metadata with liquid codec metadata while retaining the
current pure-renderer contract and fallback.

## HTTP surface

The read routes are `GET /drive`, `/drive/preview`, `/drive/download`, `/drive/history`,
`/drive/history/diff`, and `/drive/conflict`. Mutations are POST-only at `/drive/folder`,
`/drive/upload`, `/drive/rename`, `/drive/move`, `/drive/delete`, and `/drive/history/restore`.
Upload accepts `multipart/form-data`; the handler converts files to the capability's base64 input.
All other handlers convert query or form values to capability inputs and redirect back to a drive
read route after a successful change.

The consumer sync client, three-tree reconciliation, `liquid history <path>`, and first-class
local conflict values belong to [liquid #195](https://github.com/oramasearch/liquid/issues/195).
Lake uses the push and pull protocol documented in [`sync.md`](sync.md).

## Verification

`DrivePreview` and `WebUi.DrivePagesExposeBrowseMutationHistoryAndConflictControls` cover pure
format and page rendering. `Drive.BrowseMutationsDownloadsDiffsAndConflictsUseRepositoryCapabilities`
covers the capability boundary. The Postgres-backed suite, Linux image build, token gate, and live
seeded screenshot run are the release proof.
