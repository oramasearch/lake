# Drive sharing pages

**Status: implemented for lake #44 and #122; in the web app since lake#561.** Sharing keeps the
folder policy, live grants, expiring links, and signed audit outcomes defined in
[the sharing API](../api/sharing.md). The pages call those capabilities from the browser; they add
no second permission model and no page-only change. Files are in
`frontend/src/pages/repositories/`: `DriveSharePage.tsx`, `DriveShareLinkPage.tsx`,
`SharePage.tsx`, `SharePreviewPage.tsx`, `SharedWithMePage.tsx` and `share.tsx` (link state and
wording).

## Before a share is made

`/drive/share?repository_id=<id>&ref=<ref>&path=<folder>&workspace_id=<id>` begins with the
effective-access table from `drive_effective_access`. It identifies every current principal, role, allowed action,
and whether that access is direct or inherited. The delivery controls then make the four questions
visible before anything is shared:

| Question or known confusion | Screen answer |
|---|---|
| Who can access this? | The current-access table names people and organization identities. Separate People, Organization, and Link panels identify the intended recipient class. |
| What can they do? | Role choices name the actual actions: view; edit, including upload, rename, move, and delete; or share. Link write scope says that the public visitor page remains read-only. |
| Until when? | Every link requires a future UTC expiry and management shows it beside the link state. Live grants remain live until their policy changes. |
| What does revoke do? | The management table and each link detail page state that revocation prevents the link's next open and does not recall an independent downloaded copy. |
| A link is an attachment | Link controls and link details call it a continuing capability with a folder scope, role, expiry, and revocation state. |
| Folder location changes an inherited audience | The access table labels direct and inherited entries. The live-folder copy says that moves continue to apply; the capability keeps a stable liquid node id. |
| Removing a local sync selection revokes access | The page names policy and link revocation as the access controls. Local sync selection does not appear as a share mutation. |
| A downloaded copy can be recalled | The independent-copy panel and link-revoke message state that copied bytes cannot be recalled. |

The page shows its figures (who has access, active links, expired or revoked links) above the
access table. Each delivery is a card whose button opens its form in a dialog; the header offers
**Share with person** and **Create link** as well. The People and Organization dialogs (lake#585)
read `drive_share_people` and offer its people (or organizations) in a list under one field, **Person
(email or handle)** or **Organization (handle)**; the person may also type an email or handle.
They call `share_drive_folder` with `recipient` and the role, and nothing else: lake resolves the
identity and signs the grant's attestation, so the dialog has no TAAP identity or digest field. A
name lake cannot place comes back as the call's error under the field. An organization grant is
made to that named identity; the table shows the identities that actually have effective access. The Link control calls
`create_drive_share_link` with its role and required expiry, and shows the new link's address.

Lake does not expose a passcode on `create_drive_share_link`. The form says that passcodes are not
supported and that no passcode is sent or stored; it does not imply a protection the capability
cannot enforce.

## Which workspace holds the links

A folder's links live in one workspace of the repository's space (lake#350). The sharing pages
carry it as `workspace_id` in every URL and call. `/drive/share` without one first asks
`my_permissions` whether the person may share the folder (`object_type: "drive folder"`) and shows
the refusal if not. Then it opens the same page with the current workspace when it is one of the
space's, else the space's only workspace, written into the URL. When the space has several and
none is current, the page asks which one. The workspace choices are the space's
`list_workspaces` when the person may read it, else their own view of the space from `viewer`.
The management table names the workspace whose links it lists and links to the space's other
workspaces. `my_permissions` only decides what the page shows; `drive_effective_access` and the
share capabilities check again.

## Manage and inspect links

The folder page lists every link on that folder with its audience, allowed use, expiry, and active,
expired or revoked state. `/drive/share/link?…&workspace_id=<id>&link_id=<id>` is one link's page.
lake has no `get_drive_share_link`, so the page finds the link among the folder's links in
`drive_effective_access`. It repeats the four answers and offers **Revoke** only while the link is
active (`revoke_drive_share_link`); revoking never changes policy directly.

## Receive a link without signing in

`/share/<token>` opens the live shared folder, outside the shell and without a session. Folder names and **Open folder** enter a child;
file names open the registered preview, and **Download** returns an attachment with the exact
stored bytes. Unsupported preview formats retain the download action. The rendered role,
**View – browse and download**, describes those available actions.

A child folder provides **Up one folder** and **Shared folder** links. A preview provides
**Back to folder** and **Download**. All navigation keeps the bearer token and selected ref;
no control navigates to a broader repository page or asks the recipient to sign in. Navigation
stops at the shared root.

The folder page calls `resolve_drive_share_link` and the preview page `preview_drive_share_link`;
**Download** is lake's `/share/<token>/download` route, which calls `download_drive_share_link`. Every click checks
the current link role, scope, expiry, revocation, and descendant policy. A guessed sibling or
parent is refused. Revoking or expiring a link after listing also refuses the next preview and
download. The error page has no file actions; an already downloaded independent copy remains
with its recipient. The complete contract is [the sharing API](../api/sharing.md).

## Shared with me

`/shared-with-me` calls `drive_shared_with_me`. The capability lists live folders granted
directly to the signed-in TAAP identity, resolves the current folder path from its stable node id,
and returns only those folders. The page names the grantor, allowed actions, and whether that
folder still exists at main. It does not present a link as a personal grant or claim that copied
bytes are revocable.

## Verification

As a folder owner, open `/drive/share` on a space with two workspaces (the page asks) and with one
(it opens it); share with a person, create a link, open its detail page and revoke it. As a person
who cannot share, open `/drive/share` and see the refusal. As the recipient, open
`/shared-with-me`. Signed out, open a link, enter a child folder, preview a file, download it and
compare the bytes; then expire or revoke the link and repeat. `Drive.FolderSharingMatchesAuthorizeAndLinksExpire` and the
`SharedLinkScenario` scope, revocation and expiry cases cover the link rules
([../api/sharing.md](../api/sharing.md)).

## Recipient walkthrough evidence

Historical: recorded against the server-rendered pages, which the captures show.

The issue #122 native walkthrough uses a regular user's private repository with `shared/hello.txt`,
a nested child folder, binary bytes, and an empty file. Recipient requests omit both cookies and
Authorization. Traversal and supported preview return 200. Downloads compare equal with `cmp`;
sibling and parent paths, expiry after listing, and revocation between listing and download
return 403 for listing, preview, download, and the matching REST reads.

| Viewport / theme | Folder | Nested preview |
|---|---|---|
| 1280 / light | [Capture](screenshots/issue-122/1280-light-folder.png) | [Capture](screenshots/issue-122/1280-light-nested-preview.png) |
| 1280 / dark | [Capture](screenshots/issue-122/1280-dark-folder.png) | [Capture](screenshots/issue-122/1280-dark-nested-preview.png) |
| 640 / light | [Capture](screenshots/issue-122/640-light-folder.png) | [Capture](screenshots/issue-122/640-light-nested-preview.png) |
| 640 / dark | [Capture](screenshots/issue-122/640-dark-folder.png) | [Capture](screenshots/issue-122/640-dark-nested-preview.png) |

Each combination completes a browser download and compares it with the original. The seeded
`shared-link` fixture also passes folder/child/preview navigation with visible download controls in
all four combinations. Main-content controls remain visible without horizontal page overflow.

The walkthrough restarts the proof server between recipient runs to isolate the existing lake
#118 response-reuse defect: owner setup can leave a session cookie on later responses, and a
completed attachment can leave its Content-Disposition on a later preview. This issue does not
change the shared response/header path. The server still refuses out-of-scope or expired/revoked
reads regardless of an existing operator or user credential.
