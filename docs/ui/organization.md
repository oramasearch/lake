# Organization, Administration and Account Pages

The Organization, Administration and Account pages of the web app (lake#561):
`frontend/src/pages/organizations/`, `frontend/src/pages/admin/` and the signed-in pages of
`frontend/src/pages/account/` (the sign-in pages and `AuthFrame.tsx` are [identity.md](identity.md)'s).
They follow the page rules in [web-app.md](web-app.md): a header with the purpose and the primary
action, figures from the reads already made, a toolbar (search and filter chips) over each list,
creation in a dialog or a drawer, detail pages as a split, and confirmations that name what happens.
Every read, action, URL and query parameter is the one the ported page had; the search boxes filter
the rows already read.

## What each page shows

| Page | Path | Layout |
|---|---|---|
| Organizations | `/organizations` | Figures (organizations, owned, administered, invitations); a card per organization (name, your role chip, members, created, its pages); "New organization", "Join" (the invitations addressed to you) and "Convert your workspace" (typed-handle confirmation, checked against `get_customer` first) in dialogs. Members are counted per card (`list_memberships` by organization) only while there are 24 organizations or fewer |
| Organization | `/organization?id=&tab=` | Header with the name, your role chip and "Invite people" (dialog; 1 to 90 days, as before). Tabs stay in `tab=`: Overview (figures and facts), Members, Teams ("New team" dialog), Invitations, Access, SSO ("Add provider" drawer), Applications (cards), Usage (figures and the link to `/metering`), Audit (the organization's account audit, `audit {customer_id}`: its attributed central events and its workspaces' audits, with outcome chips, a Log column and Export; administrators only, lake#18) |
| Members, Teams, Invitations, Direct Grants, Effective Access | `/members`, `/teams`, `/invitations`, `/grants`, `/access` and the three entity routes | Rich tables with search and role or status chips; Effective Access adds direct and inherited counts and action chips |
| Member | `/member?id=&confirm_removal=yes` | Split: the member's effective access on the left; the membership facts, Change role and Remove member (a dialog, opened at once by `confirm_removal=yes`) on the right |
| Team | `/team?id=` | Split: the members table on the left; the team's facts on the right |
| Enterprise SSO | `/sso?organization_id=&provider_id=` | Figures; providers (the chosen one marked), issuer claims and the provisioning log (outcome chips) as cards |
| Customers | `/customers` | Figures by type; table with type chips |
| Applications | `/applications` | Figures; a card per application (workspace, users, pool, domain); "Register application" in a drawer. Users are counted per card only while there are 24 applications or fewer |
| Application and its Users, Organizations, Webhook events | `/application?id=`, `/application/users`, `/application/organizations`, `/application/events` | One header and route tabs for the four. Overview is a split (sign-in settings and links; facts and the hosted sign-in URL). Suspend asks first (there is no unsuspend). New organization in a dialog. New webhook in a drawer; its secret shows once in a highlighted callout |
| Your Activity, Audit Log | `/activity`, `/audit` | Figures by outcome; outcome chips, "Include reads" and the order are chips and buttons that change the query; principal, object and the time range sit under "Filters" (the same GET form). A coloured dot per row, relative times, object chips, the log as a chip |
| Usage | `/usage` | The usage dashboard ([usage.md](usage.md), lake#564) |
| Repository Usage by Hour | `/metering?customer_id=` | Figures (stored, transfer summed over the hours read, build time, run time); storage meters and compute facts; the limit and the estimated charge on the right; "Set storage limit" opens a dialog that keeps the unsigned-integer check |
| Secret Mounts, Secrets, Encryption Keys | `/secret-mounts`, `/secrets?mount_id=&name=`, `/encryption-keys?mount_id=` | A workspace chooser that sets `workspace_id`; tables with empty states that say what to do; Secrets is a split with the lookup and version history on the left and the mount on the right |
| Transparency Monitor, Model | `/transparency`, `/models/…` | Figures from the scan or the model's storage; the model's provenance in the side column |
| Settings | `/settings?tab=` | Tabs of cards (profile and MFA state, sessions and devices figures, passkeys, tokens, notifications) |
| Sessions and Devices | `/sessions` | This browser's session first, highlighted, with "Revoke and sign out" and "Sign out everywhere" (`sign_out_everywhere`, lake#1163) each behind a confirmation; other sessions in a table with kind and state chips; devices as cards |
| API Tokens | `/tokens` | Figures; "New token" in a drawer (the Liquid scopes can be filled in with one click); the secret shows once in a highlighted callout |
| Passkeys, MFA | `/passkeys`, `/mfa` | Cards; enrolling or replacing MFA asks for the current password and clears it after the request; the authenticator URI and recovery codes show once in a highlighted callout |

`organizations/shared.tsx` holds what the three folders share: `RoleBadge` (a role chip), `When`
(relative time, the full date on hover), `NameCell`, `ObjectChip`, `QueryTabs` (tabs kept in the
query), `HowItWorks`, `matches` and `countBy` (search and filter-chip options). `admin/usageFormat.ts`
holds the usage wording (`usageDuration`, `usageMoney`, `usageHour`).

## Deleting and Revoking (lake#645)

An owner deletes an organization from its header (**Delete organization**). The dialog lists what
goes from `delete_organization {dry_run: true}` and asks for the organization's handle; while the
organization still owns a workspace, the action stays disabled and each workspace is linked to its
deletion on Workspaces. A pending invitation has **Revoke** on the Invitations tab and on
`/invitations`; each direct grant has **Revoke** on `/grants`, and Effective Access links every
source to its direct grants. Both ask for the word `revoke`. The dialog is the shared one,
[deletion.md](deletion.md).

## How to verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` from `frontend/` (the build also runs
`check-effects` and `check-list-errors`). To look at the pages without a lake, serve them with
`npx vite` and answer `/v1/*` from Playwright's `page.route` with fixtures for the reads above; check
them at 1440 and 390 pixels wide.

## How to change

Keep each page's reads, actions and query parameters; change layout through the kit
(`frontend/src/lib/ui.tsx`, `overlay.tsx`) and the shared pieces above, not per-page CSS.
