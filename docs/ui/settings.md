# Organization and personal settings

**What.** The settings surface is rendered by pure functions in
`include/lake/api/web_ui.hpp` and `src/api/web_ui.cpp`, with data loaded through the capability
registry. It uses the shared shell, tabs, panels, standard controls, empty states, and error
markup from the design system. The tab order follows the page order below, so keyboard focus
visits navigation before the selected page content and form controls.

| Area | Route and tabs | Capability-backed content and actions |
|---|---|---|
| Organization | `GET /organization?id=<id>&tab=` `overview`, `teams`, `members`, `invitations`, `access`, `sso`, `applications`, `metering`, `audit` | Organization, team, member, invitation, effective-access, SSO, application, metering, and audit reads; invite (`create_invitation`), configure provider (`register_sso_provider`), and quota form (`set_storage_quota`) |
| Team | `GET /team?id=<id>` | Team memberships and effective access |
| Member | `GET /member?id=<id>` | Membership role, attestation evidence, effective access, `set_member_role`, and `remove_member` |
| Personal | `GET /settings?tab=` `profile`, `sessions`, `passkeys`, `tokens`, `notifications` | Profile identity, active sessions/devices, and links to the existing passkey and token capability forms |

The organization page is entered from `GET /organizations`. Each page has an empty state when
the corresponding list is empty. An empty state says what to do in the product (a link to the
form that fills it), never which endpoint to call. Registry refusals are returned as the API error
code and reason, instead of a locally invented policy message.

**Organizations page.** `GET /organizations` lists the organizations the viewer may read, with
their role and links to settings, Invite, teams, members, invitations, effective access and SSO.
For a signed-in person (not an operator credential) it also shows:

- **Invitations for you**: pending invitations addressed to their email or personal-space handle
  (`list_invitations` without a scope), each with **Join**, which posts `invitation_id` to
  `POST /invitations/accept` (`join_organization`) and opens the organization.
- **Create an organization**: name and optional handle, posted to `POST /organizations/create`
  (`create_organization_space`). The creator becomes owner and lands on the Invitations tab.
- **Turn your personal space into an organization**: says what moves (the handle, repositories,
  workspaces and apps stay; a new personal space is created) and asks the person to type the
  handle. `POST /organizations/convert` compares it with the space's handle from `get_customer`
  before calling `convert_personal_space`; a mismatch answers `400` and changes nothing.

The Invitations tab offers member, admin and owner with what each can do, and a lifetime of 1, 7
(the default) or 30 days; `/organization/invitations` turns `expires_in_days` (1 to 90) into
`expires_at`, and an explicit `expires_at` still wins. The tab lists sent invitations with their
status. The Teams tab creates a team through `POST /organization/teams` (`create_team`).

**Mutation contract.** The invitation form posts to `/organization/invitations`; the team form to
`/organization/teams`; the Organizations page forms to `/organizations/create`,
`/organizations/convert` and `/invitations/accept`; the SSO form posts to `/organization/sso`; role changes post to `/member/role`; removals post to `/member/remove`;
and quota changes post to `/metering/quota`. Every adapter calls the named registry capability, so
REST, MCP, and the settings surface use one validation, policy, and audit path.

**Dangerous changes.** A remove request first redirects to the member confirmation page. That page
states that membership and derived grants are revoked immediately, then posts a separate confirmed
request. This is an ordinary server-rendered confirmation page and works without JavaScript.

**Verify.** `ctest --test-dir build -R WebUi.SettingsPagesExposeTabsMutationsAndConfirmation
--output-on-failure` checks the renderer strings, tabs, form targets, confirmation copy, escaping,
and shared shell scripts. `WebUi.OrganizationsPageCreatesJoinsAndConvertsWithoutNamingEndpoints`
checks the Organizations page for a person and an operator, and that no organization, team,
member, invitation, grant or customer empty state names an endpoint. The HTTP flows are listed in
[`../api/organizations.md`](../api/organizations.md#verify-and-change). The seeded screenshot script covers the navigable pages in
`docs/ui/screenshots/`.
