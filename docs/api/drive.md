# Drive API

**Status: implemented for lake #8, #43, and #116.** Drive is a repository view for people who work with
folders and files. It does not create a second store: paths, blobs, changes, conflicts, refs, and
deduplication remain liquid repository values.

## Capabilities

| Capability | Authorization | Result |
|---|---|---|
| `drive_browse` | `read` on the nearest path object, falling back to the repository | Immediate children, size, last structural change, and requested ordering |
| `drive_preview` | `read` on the path | A server-rendered preview selected by extension. A text format (text, Markdown, CSV, JSON) shows the file's UTF-8 text exactly; a file in one of them that is not UTF-8 gets the `download` format instead of an altered preview (lake#1238), and `diff_drive_versions` refuses it as not text |
| `download_drive_file` | `read` on the path | `{path, blob, size, media_type, content_base64, download_url}`: the exact bytes as base64 for a file of at most 16 MiB (`null` above), the media type from the name, and the `GET /drive/download` address that streams any size (lake#587) |
| `drive_share_people` | `share` on the folder | The people a folder can be shared with by email or handle ([sharing.md](sharing.md#live-grants-and-effective-access)) |
| `create_drive_folder` | `write` on the containing folder | A new liquid change containing an empty-folder marker |
| `upload_drive_files` | `write` on the selected folder, every final destination, and every existing node being replaced (lake#962) | One liquid change containing all decoded uploads; it creates `main` in a repository with none, and any missing folders |
| `move_drive_path` | `write` on the source, every source descendant, and destination | A new liquid move preserving the stable path node |
| `delete_drive_path` | `write` on the path and every descendant | A new liquid deletion without history removal |
| `drive_history` | `read` on the path or nearest retained parent | Changes whose structural diff names `path` as its old or new path, one item per change with the path's state after it (see below) |
| `diff_drive_versions` | `read` on the path or nearest retained parent | The two immutable files and a structural text difference |
| `drive_conflict` | `read` on the path | Conflict kind, exact region, explanation, reasons, preserved values, and a text difference when registered |
| `resolve_conflict` | `write` on the path | The selected value and the Liquid change carrying its resolution record |
| `restore_drive_version` | `write` on the destination in current `main`, including any replaced descendants | A new liquid change containing the file bytes from `change_id` |

A path object has `object_type=path` and external id `<repository UUID>:<liquid node id>`. The
stable liquid tree node id means a move does not discard policy. Reads use the nearest existing
folder object, so repository policy remains the default. Move authorization checks both the
source and destination location. Repository mutations use the same store authorization callback
through REST, MCP, the compatible upload routes, and `lake` store drives.

## Nested Mutation Permissions (lake#962, LSR-096)

A write grant on a folder does not authorize its non-inheriting descendants. Uploads check every
final path after joining the selected folder with each item's relative path. An overwrite also
checks the existing node and all descendants it removes, including a folder replaced by a file.
Recursive deletion and moves check every affected source node before changing any bytes or policy
parents. One denied path returns `unauthorized` and leaves the entire batch and `main` unchanged.
Allowed sibling uploads still succeed, and missing folders and unborn `main` still work.

`RepositoryWriteAuthorizer` runs inside the repository store's mutation lock against the immutable
working tree being committed to `main`, including staged state. It uses a fresh policy cache instead
of authorization cached before the ref was read. Version restore and conflict resolution check the actual destination in `main` regardless of the caller's `ref`.
Tree-read and policy-read errors stop the write. The raw `commit_repository_files` capability,
methodology source changes, and repository-backed store drives use the same path checks; the first
empty-folder marker of a newly created drive has no existing descendants to authorize.

Keep new file mutation entry points on `repository_write_authorizer`; a callback must finish all
permission checks before mutation and must not re-enter the repository store while its lock is held.
The change adds no migration or operator schema handoff. Existing parent grants cease to permit
writes across a descendant policy cut.

To verify, run:

```sh
ctest --test-dir <build> --timeout 600 --output-on-failure \
  -R 'NestedMutationsRefuseProtectedDescendantsBeforeChangingMain|MutationChecksTheLockedTreeAndRestoreChecksMain'
```

The HTTP regression uses real repository commits and a folder writer excluded from a protected
child: direct/nested/mixed uploads, file and folder replacements, recursive deletion/move, raw
commits, unchanged refs after every denial, and an allowed sibling. The capability regression
supplies a different node in the tree at commit time and checks historical-ref restore denial.

## A Moved Folder's Access (lake#929)

A folder's access is the access of the folders it is in now, plus its own grants. Its policy object
is found by its stable node id, so its own grants follow it wherever it goes; its stored parent is
where it was when it was shared, so lake does not trust that parent on its own:

- **Every read and write checks the stored parents against the path.** The path's folders at the
  requested ref are read top down, and an object that inherits counts only when its parent is the
  object of the folder above it (the repository's for a top folder) and that object counted too. An
  object that does not inherit stands alone. Authorization uses the nearest object that counts. A
  parent left from where a folder used to be (a move through a pushed change or Data → Drives, or
  one made before lake#929), or one the folder never had, therefore costs the grants below it and
  never lends its own. `drive_browse` decides a child with its own object the same way: that object
  decides only when its parent is the folder's, otherwise the folder's object does.
- **`move_drive_path` moves the object.** When the source has its own object, every folder of the
  destination's parent gets one under the folder above it, and the source's object is put under the
  destination parent's (`Store::set_policy_object_parent`) before the change is written. If the
  change fails the object goes back. The source's direct grants stay; grants on its former folders
  stop applying and the destination's apply.
- **Sharing puts objects back.** `share_drive_folder` and `create_drive_share_link` give every
  folder on the path an object under the folder above it, putting back any whose stored parent is
  another, so a folder whose access fell back after an outside move is shared as it is now.

A policy object holds one parent for every ref, so a folder that sits in different places on two
refs is decided by the stored parent only where it matches; elsewhere its access falls back to the
nearest folder above whose object does match. `set_policy_object_parent` moves only `path` objects,
never under themselves (PostgreSQL takes one advisory lock for every such change, so two moves
cannot close a cycle).

To verify: `MovedFolderScenario.*` in `tests/scenarios/drive_test.cpp` (needs `LAKE_TEST_DB_URI`) and
`Policy.APathObjectMovesUnderAnotherParent`.

## Search Follows Every Drive Write (lake#584)

A drive write is a change like a push's, so search follows it the same way:
`create_drive_folder`, `upload_drive_files`, `move_drive_path`, `delete_drive_path`,
`restore_drive_version` and `resolve_conflict` each run `search::Service::reindex_artifacts`
after the change, which publishes the repository's branch tips again as its references name them
now ([search.md](search.md)). A failed index update is recorded in the call's audit details
(`search_index`) and never fails the write; in a switched workspace search then names the
workspace in `stale_workspaces` until its worker retries. Before lake#584 neither drive writes nor
pushes into an account's index on lake's volume reached hot search: only a release or an explicit
publication did, so a file saved through a drive was found only by a history scan.

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

`download_drive_file` and `GET /drive/download` give a file its media type from its extension
(`control::drive_media_type`, shared with workspace files): `churn.csv` is `text/csv`. The download
route sends that type only for types a browser shows as data (images, PDF, text, CSV, Markdown,
audio, video) and `application/octet-stream` otherwise, always as an attachment named after the
file.

Liquid #29's format registry is not yet on main. `DriveFormatRegistry::defaults()` is the explicit
integration point: replace its extension metadata with liquid codec metadata while retaining the
current pure-renderer contract and fallback.

## HTTP surface

Drive's pages (`/drive`, `/drive/preview`, `/drive/history`, `/drive/history/diff`,
`/drive/conflict`) are the web app's ([`../ui/web-app.md`](../ui/web-app.md)); every read and change
is the capability above (`drive_browse`, `upload_drive_files` with base64 file contents,
`move_drive_path`, `restore_drive_version`, `resolve_conflict`, …), so the app can do nothing REST
and MCP cannot. The one server route is `GET /drive/download?repository_id=&ref=&path=`, which
answers a file's exact bytes as an attachment through `download_drive_file`; without a credential
it redirects to `/sign-in`. It is described in `GET /v1/openapi.json` (operation
`drive_download`), beside `GET /drive/files/download` (`drive_files_download`).
The former HTML form handlers under `/drive/` were retired in lake#561. Conflict resolution
offers the left value, right value, an available reviewed proposal, or prepared replacement text,
and goes through `resolve_conflict`; it does not edit Liquid state directly.

The consumer sync client, three-tree reconciliation, `liquid history <path>`, and first-class
local conflict values belong to [liquid #195](https://github.com/oramasearch/liquid/issues/195).
Lake uses the push and pull protocol documented in [`sync.md`](sync.md).

## Verification

`DrivesHttp.ARepositoryDriveWriteIsSearchedDownloadedAndLoaded` covers search following drive
writes, `download_drive_file`'s bytes and `download_url`, the CSV media type and file name on the
route, and both routes in OpenAPI.

`DrivePreview` covers pure format rendering.
`Drive.BrowseMutationsDownloadsDiffsAndConflictsUseRepositoryCapabilities` covers the capability
boundary. The Postgres-backed suite, Linux image build, token gate, and live
seeded screenshot run are the release proof.

## Upload input schema

`upload_drive_files` takes `repository_id`, `ref`, `path`, and a nonempty `files` array.
Each item is an object with `content` (padded base64 file bytes, required) and either `path`
(relative to the folder; it may name folders, `Q3/report.csv`) or `name` (one file name within
the folder), never both (`invalid_input` at `/files/<n>/path`); `content_type` is accepted so
the item has the same shape as `upload_drive_entries`' (lake#587), and a repository keeps no media
type. An empty `content` string creates an empty file; additional item fields are rejected. The registry advertises these nested fields and
`contentEncoding: base64` in both OpenAPI and MCP.

The target folder need not exist (lake#560): an upload into `Reports/2026` when neither folder is
there creates both, as a file manager would, and the policy check is `write` on the nearest folder
that does exist (the repository itself at the top). A repository nobody has written to yet (made
by `create_repository`, with no ref at all) has no `main`; an upload to `ref: main` treats it as
empty and its change creates `main`. Every other drive capability still answers `not_found` for
such a repository, and a ref other than `main` is never created. An upload whose `path` names a
file is refused (`invalid_input`, "path component is not a directory"). Before lake#560 the first case answered `not_found` "no revision main" and the
second `not_found` "no drive path <path>", both from the policy check, before anything was
written. The tests are `DrivesHttp.TheFirstUploadCreatesMainAndAMissingFolder` (memory store),
`DriveScenario.TheFirstUploadCreatesMainAndAMissingFolder` and
`DriveScenario.AnUploadIntoAMissingFolderCreatesIt` (Postgres, signed in as the owner, which is
where both failed) and
`WorkspaceStoreDrive.TheFirstUpload*` (the account store and a switched workspace store), one
exercise in `tests/drive_history_support.hpp`.

Registry validation rejects wrong item and field types before the handler runs. For example,
`files: [1]` returns REST `400 invalid_input` with `error.path: "/files/0"`; a missing or
non-string content field identifies `/files/0/content`, and so does an empty item, since
`content` is the one field every item needs. Invalid base64 uses the same field path.
MCP carries the same error under its documented [response envelope](README.md).

Verify with the HTTP schema conformance test described in [the API contract](README.md).
When changing upload fields, update the registry item schema, byte decoder when needed, and HTTP
malformed-input cases together.
