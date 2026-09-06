# Drive sharing pages

**Status: implemented for lake #44.** Sharing keeps the folder policy, live grants, expiring
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
