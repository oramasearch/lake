# Drive sharing

**Status: implemented for lake #8, #44, and #122.** A folder can be delivered as a live grant, a scoped
link, or an independent copy. Lake implements live grants and revocable links. The sharing pages
show effective access before creation, link expiry and state, the exact revocation consequence,
and folders shared directly with the signed-in identity. A copy is independent bytes which cannot
be recalled; export or download performs the copy.

## Live grants and effective access

`share_drive_folder` accepts `repository_id`, `ref`, a folder `path`, recipient TAAP id, role, and
attestation digest. It creates the path policy object when needed, then writes an ordinary policy
grant. The recipient sees the continuing live object. Other principals receive `unauthorized`.

`drive_effective_access` calls the policy engine's `effective_access` function and returns each
principal, role, allowed actions, source object, and `inherited` flag. The sharing page renders
that result directly, so its answers match `authorize` rather than duplicating role logic.

## Links

`create_drive_share_link` requires a folder, `read` or `write` role, and future RFC 3339 expiry.
Lake returns the bearer token once and persists only its HMAC-SHA-256 digest. The durable
`drive_share_links` row identifies the repository, folder object, stable node id, path, role,
expiry, creator, and revocation. A synthetic `share-link:<id>` grant makes the policy engine the
authority for link scope and role.

All recipient reads are public registry capabilities. They accept the bearer `token` and a `ref`;
they require no session and create no repository or personal grant. An existing signed-in or
operator session cannot add authority to a link.

| Capability | Additional input | Result |
|---|---|---|
| `resolve_drive_share_link` | Optional repository-relative `path`; omit it to open the shared root | Immediate children of the selected folder, its current `path`, and `root_path` |
| `preview_drive_share_link` | Required repository-relative file `path` | `path`, `root_path`, `blob`, `size`, `format`, `media_type`, and format-registry `html`; unsupported formats show a download fallback |
| `download_drive_share_link` | Required repository-relative file `path` | `path`, `root_path`, `blob`, `size`, `media_type`, and exact stored bytes in standard padded `content_base64` |

The REST reads are `GET /v1/<capability>` with query parameters. OpenAPI describes the same inputs
and outputs, including `contentEncoding: base64`, and MCP lists and calls the same capabilities.
Decoding the download's base64 produces byte-identical content, including empty files, NUL bytes,
and non-UTF-8 bytes. Browser routes render these results and offer an attachment download; they
never obtain an independent repository credential.

Drive-link rows render expiry in canonical UTC with microseconds, independent of the Postgres
server timezone; active-link comparisons normalize optional fractions to the same precision.
Every read looks up the durable link, checks expiry and revocation, requires a `read` or `write`
link role, and authorizes the synthetic principal with the policy engine. It resolves the shared
root from its stable node at the requested ref, then requires the requested path to be that root
or a descendant separated by `/`. It authorizes the nearest path policy object within the shared
root; an explicitly restricted descendant remains restricted and is hidden from the parent list.
A folder move follows the same stable node. Old locations, guessed siblings (including names
starting with the shared folder name), parents, absolute paths, empty explicit paths, dot
components, doubled separators, backslashes, and NUL bytes are refused. The omitted root path is
different from an explicit empty path. Folder listings require folders; byte reads require blobs.
File bytes come from the digest selected by the authorized tree, so a ref changing during the call
cannot substitute content from a different path.

`revoke_drive_share_link` stops the next list, preview, or download, including a download clicked
from a page opened before revocation. Expiry has the same effect. Browser responses use `no-store`.
These controls cannot recall a copy already delivered. The recipient routes are
`GET /share/:token[?ref=main&path=...]`, `/share/:token/preview`, and `/share/:token/download`;
preview and download require `path`. Create and revoke remain POSTs under `/drive/share/`.

`drive_shared_with_me` is an authenticated read capability. It lists only `path` objects with a
direct grant to the caller's TAAP identity, resolves their current `main` path by stable node id,
and returns the repository, grantor, role-derived actions, and availability. It does not treat a
bearer link or an inherited repository permission as a direct folder share.

`create_drive_share_link` has no passcode input. The UI names that limitation rather than sending
or storing a passcode that the capability cannot verify.

Every capability outcome is written through the signed registry audit path. Successful changes
use `drive.folder.shared`, `drive.share_link.created`, and `drive.share_link.revoked` audit kinds.

## Verification

`Drive.FolderSharingMatchesAuthorizeAndLinksExpire` proves recipient visibility, outsider denial,
effective-access agreement with `authorize`, a recipient's shared-with-me result, active-link
resolution after a stable-node move, registry previews/downloads, OpenAPI/MCP discovery, MCP byte
delivery, scope refusals even with an operator context, revocation, and expiry.
`Drive.ShareDownloadsEncodeAcrossBlockBoundaries` checks binary bytes and padding across the
base64 encoder block boundary. `PostgresCapabilities.DriveShareLinksRoundTrip` covers durable
create, UTC expiry, lookup, list, and revoke.

`SharedLinkScenario.*` runs the real Postgres-backed customer path: a regular user's private
repository, signed-out descendant traversal and preview, exact text/binary/empty downloads,
read and write link roles, sibling/parent refusal, restricted descendant policy, expiry, and
revocation between listing and download. Run the full native suite with `LAKE_TEST_DB_URI` and
`LAKE_INSTANCE_KEK` as described in [the local build guide](../build/local.md).

When changing recipient delivery, keep all three operations on `shared_drive_target` and their
registry schemas; extend the scope/refusal scenario alongside any new path or delivery behavior.
