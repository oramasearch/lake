# Drive sharing pages

**Status: implemented for lake #44 and #122.** Sharing keeps the folder policy, live grants, expiring
links, and signed audit outcomes defined in [the sharing API](../api/sharing.md). The pages do not
introduce a second permission model or a browser-only mutation.

## Before a share is made

GET /drive/share?repository_id=<id>&ref=<ref>&path=<folder> begins with the effective-access
table from drive_effective_access. It identifies every current principal, role, allowed action,
and whether that access is direct or inherited. The delivery controls then make the four questions
visible before any form is submitted:

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

The People and Organization controls both call share_drive_folder with the named TAAP identity,
role, and attestation digest. An organization grant is made to that named identity; the table
shows the identities that actually have effective access. The Link control calls
create_drive_share_link with its role and required expiry.

Lake does not currently expose a passcode field on create_drive_share_link. The form therefore
shows a disabled passcode control and says that no passcode is sent or stored. It does not imply a
passcode protection that the capability cannot enforce.

## Manage and inspect links

The folder page lists every link on that folder with its audience, allowed use, expiry, and active,
expired, or revoked state. GET /drive/share/link?...&link_id=<id> is the individual link page.
It repeats the four answers and provides POST /drive/share/revoke only while the link is active.
That route calls revoke_drive_share_link; it never changes policy directly.

## Receive a link without signing in

`GET /share/<token>` opens the live shared folder. Folder names and **Open folder** enter a child;
file names open the registered preview, and **Download** returns an attachment with the exact
stored bytes. Unsupported preview formats retain the download action. The rendered role,
**View – browse and download**, describes those available actions.

A child folder provides **Up one folder** and **Shared folder** links. A preview provides
**Back to folder** and **Download**. All navigation keeps the bearer token and selected ref;
no control navigates to a broader repository page or asks the recipient to sign in. Navigation
stops at the shared root. Table wrapping and the existing application shell support 1280 and
640 pixel viewports in light and dark themes without a page script.

The page calls `resolve_drive_share_link`; preview and attachment routes call
`preview_drive_share_link` and `download_drive_share_link` through the registry. Every click checks
the current link role, scope, expiry, revocation, and descendant policy. A guessed sibling or
parent is refused. Revoking or expiring a link after listing also refuses the next preview and
download. The error page has no file actions; an already downloaded independent copy remains
with its recipient. The complete contract is [the sharing API](../api/sharing.md).

## Shared with me

GET /shared-with-me calls drive_shared_with_me. The capability lists live folders granted
directly to the signed-in TAAP identity, resolves the current folder path from its stable node id,
and returns only those folders. The page names the grantor, allowed actions, and whether that
folder still exists at main. It does not present a link as a personal grant or claim that copied
bytes are revocable.

## Verification

WebUi.DriveSharingAnswersAccessExpiryAndRevocationQuestions asserts the four questions, passcode
limitation, revocation consequence, route forms, and escaped sharing values.
Drive.FolderSharingMatchesAuthorizeAndLinksExpire also proves the recipient's
drive_shared_with_me result. The seeded fixture captures the folder sharing page, an individual
link page, and a recipient's shared-with-me page in both themes through the production HTTP
server.

`WebUi.SharedLinkProvidesScopedNavigationPreviewAndDownload` checks escaped links, descendant
navigation, preview/download controls, and the error state. `SharedLinkScenario.*` covers those
routes through a real Postgres-backed server without a recipient session. The `shared-link`
seeded page includes a nested child folder and uses the production recipient renderer. Verify its
folder and preview pages at 1280 and 640 pixels in both themes, and compare the downloaded bytes
with the original. Repeat the preview and download after expiry and revocation.


## Recipient walkthrough evidence

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
