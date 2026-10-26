# Organization and personal settings

**What.** The settings pages of the web app read and change organizations, teams, members and the
person's own account through the registry's capabilities, called from the browser. The tab order
follows the page order below, so keyboard focus visits the tabs before the selected tab's content
and forms. Files are under `frontend/src/pages/organizations/` and `frontend/src/pages/account/`.

| Area | Route and tabs | File | Content and actions |
|---|---|---|---|
| Organization | `/organization?id=<id>&tab=` `overview`, `teams`, `members`, `invitations`, `access`, `sso`, `applications`, `metering` (labelled Usage), `audit` | `OrganizationPage.tsx` | `get_organization`, `list_teams`, `list_memberships`, `list_invitations`, `entity_effective_access`, `list_sso_providers`, `list_applications`, `metering_usage` (Usage tab only, because it reads the account's workspaces) and `audit {customer_id}` (the account's audit, its administrators only, lake#18) reads; invite (`create_invitation`), create a team (`create_team`), configure a provider (`register_sso_provider`) |
| Team | `/team?id=<id>` | `TeamPage.tsx` | the team's memberships and effective access |
| Member | `/member?id=<id>` | `MemberPage.tsx` | role, attestation evidence, effective access, `set_member_role`, and confirmed `remove_member` |
| Usage | `/usage` | `../usage/UsagePage.tsx` | usage over time across the chosen workspaces, limits and CSV ([usage.md](usage.md), lake#564) |
| Repository Usage by Hour | `/metering?customer_id=<id>` | `../admin/MeteringPage.tsx` | the account's repository usage by hour and its repository storage limit (`set_storage_quota`); the organization's Usage tab and the Usage dashboard link here |
| Personal | `/settings?tab=` `profile`, `sessions`, `passkeys`, `tokens`, `notifications` | `../account/SettingsPage.tsx` | see [Personal settings](#personal-settings) |

Each list has an empty state that says what to do in the product (a link to the form that fills
it), never which endpoint to call. A capability's refusal is shown as its error code and reason,
not a policy message invented by the page.

**Organizations page.** `/organizations` (`OrganizationsPage.tsx`) lists the organizations the
viewer may read, with their role and links to settings, Invite, teams, members, invitations,
effective access and SSO. For a signed-in person (not the operator credential) it also shows:

- **Invitations for you**: pending invitations addressed to their email or personal-account handle
  (`list_invitations` without a scope), each with **Join** (`join_organization`), which opens the
  organization.
- **Create an organization**: name and optional handle (`create_organization_space`). The creator
  becomes owner and lands on the Invitations tab.
- **Turn your personal account into an organization**: says what moves (the handle, repositories,
  workspaces and apps stay; a new, empty personal account is created) and asks the person to type
  the handle. The page reads the account's current handle with `get_customer` and compares it before
  calling `convert_personal_space`; a mismatch changes nothing.

**Invitations.** The Invitations tab offers member, admin and owner with what each can do, and a
lifetime of 1, 7 (the default) or 30 days. The page turns the day count (1 to 90) into
`expires_at`, in UTC to the second, before calling `create_invitation`. The tab lists sent
invitations with their status. `list_invitations` is for administrators: a plain member sees its
refusal inside the Invitations panel and the rest of the page still opens (before lake#561 the
whole page answered 403).

**Dangerous changes.** **Remove** on a member opens `/member?id=<id>&confirm_removal=yes`, which
states that the membership and the grants derived from it are revoked immediately, and removes the
member only from that confirmation.

## Personal settings

`/settings` has six tabs; an unknown `tab` shows Notifications. The breadcrumb is Account › the
tab. Profile shows the person's TAAP identity (from `viewer`), linking to Identity; Identity
(`IdentityPanel.tsx`, lake#762) reads `account_identity` (managed key or own key, the account
identity and what it signs as) and, for a managed key, offers **Use my own identity**: paste
`liquid identity show --document` (`begin_own_identity`), run the `liquid sign artifact` command it
shows and paste the output (`use_own_identity`); a refused signature starts again from the document
([identity.md](../api/identity.md#your-own-identity)); Sessions and devices reads
`list_sessions` and `list_devices`; Passkeys and Tokens link to `/passkeys` and `/tokens`, where the capability forms
are ([identity.md](identity.md)); Notifications lists what Lake told the current workspace about failed apps, failed builds and auto-fix that stopped, with Fix, Open and Dismiss ([apps.md](apps.md#when-an-app-fails), lake#1207).

### Deleting your account

Profile ends with **Delete account** (lake#810): the shared confirmation
([deletion.md](deletion.md)) over `delete_account` with no `user_id`, so it is always the signed-in
person's own account. The dry run lists what goes (`accountLines` in `frontend/src/lib/deletion.ts`:
workspaces with those of the organizations going, the organizations deleted and those left to their
other owners, sessions and tokens, credentials and devices, grants, the billing contact) and says
that invoices and the audit log are kept. While you are the only owner of an organization other
people belong to, the action is disabled and each such organization is linked, with how many others
are in it (`accountBlockers`); the page never sends `delete_organizations`. Typing your handle enables
it; Lake refuses it unless you signed in within the last 15 minutes (the dialog shows the refusal,
which says to sign in again). Once it is done the page signs out (`POST /logout`), since every
session went with the account. The capability is [identity.md](../api/identity.md#deleting-an-account).

**What changed.** Every form posted to a server adapter (`/organization/invitations`,
`/organization/teams`, `/organization/sso`, `/organizations/create`, `/organizations/convert`,
`/invitations/accept`, `/member/role`, `/member/remove`, `/metering/quota`) is now the page
calling that adapter's capability as `POST /v1/<name>`. The checks the adapters made before the
call (the typed handle, the invitation lifetime, the removal confirmation) are made by the page.

**Verify.** Sign in as an organization owner and as a plain member, open `/organization?id=…` on
every tab, invite someone with each lifetime, create a team, change a member's role and remove
them through the confirmation. As a person with a personal account, create an organization, join an
invitation, and convert the personal account with a wrong handle (nothing changes) and then the right
one. The capability flows are tested in
[`../api/organizations.md`](../api/organizations.md#verify-and-change).

**Change.** A new tab is an entry in `TABS` in `OrganizationPage.tsx` and a component reading its
capability; a new action is a capability first.
