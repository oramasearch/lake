# Drive sharing

**Status: implemented for lake #8, #44, and #122.** A folder can be delivered as a live grant, a scoped
link, or an independent copy. Lake implements live grants and revocable links. The sharing pages
show effective access before creation, link expiry and state, the exact revocation consequence,
and folders shared directly with the signed-in identity. A copy is independent bytes which cannot
be recalled; export or download performs the copy.

## Live grants and effective access

`share_drive_folder` accepts `repository_id`, `ref`, a folder `path`, the recipient and a `role`
(`read`, `write` or `share`). It creates the path policy object when needed, then writes an
ordinary policy grant. The recipient sees the continuing live object. Other principals receive
`unauthorized`.

### Naming the Recipient (lake#585)

- **`recipient`**, for people: an email or a handle (`ana` or `@ana`), matched without regard to
  case among the space's people. Those are what `drive_share_people` lists: the space's owner, and
  for an organization the organization itself (by its handle) and every user who holds one of its
  memberships, each with `taap_id`, `kind`, `display_name`, `email` and `handle` (their personal
  space's name). Anyone else is `invalid_input` at `/recipient`, which says to invite them first.
  `drive_share_people` needs `share` on the folder, as sharing does, so only someone who may share
  sees the members' emails.
- **`principal_taap_id`**, for programs: a TAAP identity, which lake must know as a user or an
  organization (`principal_names`); an unknown one is `invalid_input` at `/principal_taap_id`.
- Exactly one of the two (`invalid_input` at `/recipient` otherwise).

### The Grant's Attestation (lake#585)

The grant's `attestation_digest` is evidence of the grant, so lake makes it rather than taking a
caller's word. For each share lake writes the statement

`{type: "lake.drive.share/1", repository_id, object_id, node_id, path, principal_taap_id, role,
granted_by, issuer}`

signs its JSON with lake's own TAAP identity (`audit::sign_taap`, purpose `drive-share`, the same
key and signing path as the audit stream), and keeps `sha256:<hex>` of the canonical JSON of
`{statement, signer, signature}` on the grant. The answer and the `drive.folder.shared` audit event
carry `attestation: {statement, signer, signature, digest}`, so anyone holding the audit can
recompute the digest and check the signature against lake's identity document.

The statement has no time in it and Ed25519 signatures are deterministic, so the same share under
the same lake identity always has the same digest. `attestation_digest` is therefore optional, and
accepted only when it equals that digest (a program repeating a share it made); any other value is
`invalid_input` at `/attestation_digest`, saying lake makes the attestation. Before lake#585 the
caller supplied the digest and lake stored whatever it was sent. Sharing what is already shared
answers the existing grant with `existing: true` instead of `conflict`.

After lake rotates its identity, a repeated share's digest differs; such a program leaves
`attestation_digest` out.

`drive_effective_access` calls the policy engine's `effective_access` function and returns each
principal, role, allowed actions, source object, and `inherited` flag. The sharing page renders
that result directly, so its answers match `authorize` rather than duplicating role logic.

## Links

`create_drive_share_link` requires a folder, `workspace_id`, `read` or `write` role, and future
RFC 3339 expiry. Lake returns the bearer token once and keeps only its HMAC-SHA-256 digest. A
synthetic `share-link:<id>` grant makes the policy engine the authority for link scope and role.

### Where links live

A link lives in the workspace Zetta of the workspace that holds it (lake#350), as one sealed record
of that Zetta's `lake_drive_share_links` collection
([workspace records](../architecture/workspace-records.md)). The record holds the repository,
folder object, stable node id, path, role, token digest, expiry, creator and revocation. The
workspace is chosen by the caller and never by default: `workspace_id` must name a live workspace
of the repository's space, or the call is `invalid_input` at `/workspace_id`. Ownership is
immutable. The record is sealed to its workspace and key, so a copy in another workspace does not
open, and a link can only be revoked or listed through the workspace that holds it.

| Capability | Workspace |
|---|---|
| `create_drive_share_link` | `workspace_id` (required) |
| `drive_effective_access` | `workspace_id` (required): lists that workspace's links on the folder |
| `revoke_drive_share_link` | `workspace_id` and `link_id` (both required); `not_found` at `/link_id` in any other workspace |
| `resolve_drive_share_link`, `preview_drive_share_link`, `download_drive_share_link` | The bearer token's |

A recipient may not be signed in and may hold only the token, so the token routes itself: it is
`<workspace id>.<link id>.<secret>`. Resolving it reads the workspace's row in the multitenancy
tables (which space owns it, and whether it is live), then reads the one record by key from that
workspace's Zetta, and compares the token's digest with the sealed one in constant time. No central
lookup is needed, and none holds link content. A token that names another workspace or link, or
has another secret, is `not_found` at `/token` without saying which.

Lake's PostgreSQL keeps only:

- the `share-link:<id>` grant (policy, which stays central);
- `drive_share_link_routes`, the dispatch entry of a token issued before lake#350. Those tokens do
  not name their workspace. Each route holds the token digest, link id and workspace id, and no
  path, role, expiry or creator. Migration `0048_drive_share_link_workspaces` and the copy write
  it for each link whose workspace is known. A route is removed with its workspace;
- nothing else: the retired `drive_share_links` table was copied and verified on dev, and
  migration `0070_drop_moved_tables` drops it (lake#311).

A workspace that is not ready answers `workspace_not_ready` instead of falling back anywhere.
Deleting a workspace deletes its Zetta and the links in it; their tokens then answer `not_found`.

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

Expiry is kept as given at creation (a copied link keeps PostgreSQL's canonical UTC with
microseconds); active-link comparisons normalize optional fractions to the same precision.
Every read looks up the link in its workspace, checks expiry and revocation, requires a `read` or `write`
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
These controls cannot recall a copy already delivered. A recipient opens
`/share/:token[?ref=main&path=...]` and `/share/:token/preview` in the web app, which calls
`resolve_drive_share_link` and `preview_drive_share_link`; `GET /share/:token/download` is the
server's bytes route. Preview and download require `path`. Create and revoke are the
`create_drive_share_link` and `revoke_drive_share_link` capabilities.

`drive_shared_with_me` is an authenticated read capability. It lists only `path` objects with a
direct grant to the caller's TAAP identity, resolves their current `main` path by stable node id,
and returns the repository, grantor, role-derived actions, and availability. It does not treat a
bearer link or an inherited repository permission as a direct folder share.

`create_drive_share_link` has no passcode input. The UI names that limitation rather than sending
or storing a passcode that the capability cannot verify.

Every capability outcome is written through the signed registry audit path. Successful changes
use `drive.folder.shared`, `drive.share_link.created`, and `drive.share_link.revoked` audit kinds.

## Verification

`Drive.SharingByEmailOrHandleIsAttestedByLake` shares by email (any case) and by handle among an
organization's people, checks the lake-signed statement and its digest on the grant, accepts the
answered digest for the same share (the existing grant), refuses a made-up digest and a digest for
another role at `/attestation_digest`, refuses an outsider's email at `/recipient` and an unknown
TAAP identity at `/principal_taap_id`, and accepts a known one.
`Drive.FolderSharingMatchesAuthorizeAndLinksExpire` proves recipient visibility, outsider denial,
effective-access agreement with `authorize`, a recipient's shared-with-me result, active-link
resolution after a stable-node move, registry previews/downloads, OpenAPI/MCP discovery, MCP byte
delivery, scope refusals even with an operator context, revocation, and expiry.
`Drive.ShareDownloadsEncodeAcrossBlockBoundaries` checks binary bytes and padding across the
base64 encoder block boundary. `Drive.ShareLinksLiveOnlyInTheWorkspaceThatHoldsThem` covers two
spaces with two workspaces each: the record only at its workspace's endpoint and sealed, missing,
malformed, unknown and foreign `workspace_id`, least-privilege callers, forged tokens and a sealed
record moved to another workspace, a workspace that is not ready, revocation and its conflict, and
workspace deletion. `PostgresDriveShareLinks.MigrationRecordsOwnersAndContentFreeRoutes` covers
migration `0048`, and `Stores/WorkspaceRecordStores.TheOneShotCopyMovesDriveShareLinksAndRoutesTheirTokens`
the copy over both stores.

`SharedLinkScenario.*` runs the real Postgres-backed customer path: a regular user's private
repository, signed-out descendant traversal and preview, exact text/binary/empty downloads,
read and write link roles, sibling/parent refusal, restricted descendant policy, expiry, and
revocation between listing and download. Run the full native suite with `LAKE_TEST_DB_URI` and
`LAKE_INSTANCE_KEK` as described in [the local build guide](../build/local.md).

When changing recipient delivery, keep all three operations on `shared_drive_target` and their
registry schemas; extend the scope/refusal scenario alongside any new path or delivery behavior.
