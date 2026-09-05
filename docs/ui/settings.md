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
the corresponding list is empty. Registry refusals are returned as the API error code and reason,
instead of a locally invented policy message.

**Mutation contract.** The invitation form posts to `/organization/invitations`; the SSO form posts
to `/organization/sso`; role changes post to `/member/role`; removals post to `/member/remove`;
and quota changes post to `/metering/quota`. Every adapter calls the named registry capability, so
REST, MCP, and the settings surface use one validation, policy, and audit path.

**Dangerous changes.** A remove request first redirects to the member confirmation page. That page
states that membership and derived grants are revoked immediately, then posts a separate confirmed
request. This is an ordinary server-rendered confirmation page and works without JavaScript.

**Verify.** `ctest --test-dir build -R WebUi.SettingsPagesExposeTabsMutationsAndConfirmation
--output-on-failure` checks the renderer strings, tabs, form targets, confirmation copy, escaping,
and shared shell scripts. The seeded screenshot script covers the navigable pages in
`docs/ui/screenshots/`.
