# Drive API

**Status: implemented for lake #8, #43, and #116.** Drive is a repository view for people who work with
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
| `drive_history` | `read` on the path or nearest retained parent | Changes whose structural diff names `path` as its old or new path, one item per change with the path's state after it (see below) |
| `diff_drive_versions` | `read` on the path or nearest retained parent | The two immutable files and a structural text difference |
| `drive_conflict` | `read` on the path | Conflict kind, exact region, explanation, reasons, preserved values, and a text difference when registered |
| `resolve_conflict` | `write` on the path | The selected value and the Liquid change carrying its resolution record |
| `restore_drive_version` | `write` on the path or nearest retained parent | A new liquid change containing the file bytes from `change_id` |

A path object has `object_type=path` and external id `<repository UUID>:<liquid node id>`. The
stable liquid tree node id means a move does not discard policy. Reads use the nearest existing
folder object, so repository policy remains the default. Move authorization checks both the
source and destination location. Every mutation calls `commit_repository_files`; the HTTP forms
do not write the store directly.

## Path history

`drive_history` answers one item per change, newest first, with `changes`, `old_blob`, `new_blob`,
`old_path` and the conflict identities. A change can carry several structural entries for one path:
`upload_drive_files` over an existing file removes the old node and adds a new one at the same
path. The item is the path's state after the change (lake#123): the entry that leaves a file at
`path`, with `old_blob` taken from the entry that held `path` before and `added` reported as
`modified`. A restore, which rewrites the same node, is a plain `modified` entry. An item with an
empty `new_blob` is a deletion and has no version to compare or restore.
`diff_drive_versions` and `restore_drive_version` take the item `id` as `left`/`right` or
`change_id`.

## Formats

The built-in registry covers plain source and configuration text, Markdown, PNG/JPEG/GIF/WebP,
CSV, JSON, PDF, MP4/WebM/Ogg video, and MP3/M4A/Ogg/WAV audio. Markdown and all tabular values are
escaped before becoming HTML. PDF and media bytes are supplied to browser-native elements.
Unknown extensions return the `download` presentation with `application/octet-stream`.

Liquid #29's format registry is not yet on main. `DriveFormatRegistry::defaults()` is the explicit
integration point: replace its extension metadata with liquid codec metadata while retaining the
current pure-renderer contract and fallback.

## HTTP surface

Drive's pages (`/drive`, `/drive/preview`, `/drive/history`, `/drive/history/diff`,
`/drive/conflict`) are the web app's ([`../ui/web-app.md`](../ui/web-app.md)); every read and change
is the capability above (`drive_browse`, `upload_drive_files` with base64 file contents,
`move_drive_path`, `restore_drive_version`, `resolve_conflict`, …), so the app can do nothing REST
and MCP cannot. The one server route is `GET /drive/download`, which answers a file's exact bytes
as an attachment through `download_drive_file`; without a credential it redirects to `/sign-in`.
The former HTML form handlers under `/drive/` were retired in lake#561. Conflict resolution
offers the left value, right value, an available reviewed proposal, or prepared replacement text,
and goes through `resolve_conflict`; it does not edit Liquid state directly.

The consumer sync client, three-tree reconciliation, `liquid history <path>`, and first-class
local conflict values belong to [liquid #195](https://github.com/oramasearch/liquid/issues/195).
Lake uses the push and pull protocol documented in [`sync.md`](sync.md).

## Verification

`DrivePreview` covers pure format rendering.
`Drive.BrowseMutationsDownloadsDiffsAndConflictsUseRepositoryCapabilities` covers the capability
boundary. The Postgres-backed suite, Linux image build, token gate, and live
seeded screenshot run are the release proof.

## Upload input schema

`upload_drive_files` takes `repository_id`, `ref`, `path`, and a nonempty `files` array.
Each item is an object with required string fields `name` (one file name within the folder)
and `content` (padded base64 file bytes). An empty `content` string creates an empty file;
additional item fields are rejected. The registry advertises these nested fields and
`contentEncoding: base64` in both OpenAPI and MCP.

Registry validation rejects wrong item and field types before the handler runs. For example,
`files: [1]` returns REST `400 invalid_input` with `error.path: "/files/0"`; a missing or
non-string content field identifies `/files/0/content`. Invalid base64 uses the same field path.
MCP carries the same error under its documented [response envelope](README.md).

Verify with the HTTP schema conformance test described in [the API contract](README.md).
When changing upload fields, update the registry item schema, byte decoder when needed, and HTTP
malformed-input cases together.
