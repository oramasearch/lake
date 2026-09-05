# Drive API

**Status: implemented for lake #8 and #43.** Drive is a repository view for people who work with
folders and files. It does not create a second store: paths, blobs, changes, conflicts, refs, and
deduplication remain liquid repository values.

## Capabilities

| Capability | Authorization | Result |
|---|---|---|
| `drive_browse` | `read` on the nearest path object, falling back to the repository | Immediate children of `repository_id`, `ref`, and optional `path` |
| `drive_preview` | `read` on the path | A server-rendered preview selected by extension |
| `drive_history` | `read` on the path | Changes whose structural diff names `path` as its old or new path |
| `restore_drive_version` | `write` on the path | A new liquid change containing the file bytes from `change_id` |

A path object has `object_type=path` and external id `<repository UUID>:<liquid node id>`. The
stable liquid tree node id means a move does not discard policy. Sharing creates missing folder
objects from the repository down to the selected folder. Reads use the nearest existing folder
object, so repository policy remains the default.

The format registry is extendable with one renderer per extension. The default set renders plain
text, Markdown, PNG/JPEG/GIF/WebP images, CSV and JSON tables, and PDF metadata. Renderers return
HTML fragments to the pure page renderer; none run browser code. Unsupported extensions return
`invalid_input` rather than guessing.

`GET /drive`, `/drive/preview`, and `/drive/history` are the server-rendered surface. Restore is a
POST. Conflicts continue to use liquid's multiple blob values and the existing conflicts page; no
version is overwritten or hidden.

The consumer sync client, three-tree reconciliation, `liquid history <path>`, and first-class
local conflict values belong to [liquid #195](https://github.com/oramasearch/liquid/issues/195).
Lake uses the push and pull protocol documented in [`sync.md`](sync.md).

## Verification

`DrivePreview.DefaultRegistryRendersEveryFormat` covers the registry. `Drive` capability tests
cover scoped visibility, history filtering, and restore-as-a-new-change. The full proof remains
`cmake --build build && ctest --test-dir build` plus the Linux image build.
