# Drive sharing

**Status: implemented for lake #8 and #44.** A folder can be delivered as a live grant, a scoped
link, or an independent copy. Lake implements live grants and revocable links. The page explains
that a copy is independent bytes which cannot be recalled; export or download performs the copy.

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

`resolve_drive_share_link` is the public entry capability. It rejects expired or revoked rows,
calls `authorize` for the synthetic principal, and returns only that folder's immediate children.
`revoke_drive_share_link` takes effect on the next open. The browser route is `GET /share/:token`;
create and revoke remain POSTs under `/drive/share/`.

Every capability outcome is written through the signed registry audit path. Successful changes
use `drive.folder.shared`, `drive.share_link.created`, and `drive.share_link.revoked` audit kinds.

## Verification

`Drive.FolderSharingMatchesAuthorizeAndLinksExpire` proves recipient visibility, outsider denial,
effective-access agreement with `authorize`, active-link resolution, revocation, and expiry.
`PostgresCapabilities.DriveShareLinksRoundTrip` covers durable create, lookup, list, and revoke.
