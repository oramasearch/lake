# Organization

The account holder's Organization group: Organizations, an organization's settings and its tabs,
teams, members, invitations, direct grants, effective access, Enterprise SSO, Secrets
(`/secret-mounts`, `/secrets`) and Your Activity. This file makes `TD-ORG` and `TD-SECRET`, so run
it before any file that needs them. The operator's view of these pages is in
[16-administration.md](16-administration.md).

## Before you start

- Personas: `owner`, `member`, `writer`, `newcomer`, `operator`, and a spare Clerk user `spare`
  (optional, only for ORG-047). Each needs an inbox the team controls.
- Test data: `W1`, `TD-ORGWS` (made during this file, after ORG-015), `TD-REPO` (for ORG-030).
- This file makes:
  - `TD-ORG`: the organization `qa-<MMDD>-org` (ORG-002), with `member` (ORG-007) and the team
    `qa-team` (ORG-015).
  - `TD-SECRET`: the mount `qa-<MMDD>-mount` in `W1` with the secret `crm` holding the field
    `token` (ORG-049, ORG-052).
  - Throwaway objects, all deleted here: `qa-<MMDD>-org2`, `qa-<MMDD>-mount2`, the secret
    `qa-destroy`, an invitation to `qa-<MMDD>-nobody@<test inbox domain>`.
- Lake sends no invitation email. A person joins by signing in with the invited email address and
  opening **Organization → Organizations**.
- ORG-036 and ORG-038 need an API token for `owner` (**Account → API Tokens**).
- After ORG-015, make `TD-ORGWS` (**Ship → Workspaces → New workspace**, owner `TD-ORG`) before
  ORG-041.

## Organizations

#### ORG-001 · A newcomer sees the empty Organizations page · P1
As: newcomer

1. Open **Organization → Organizations**.

Expect:
- Title "Organizations", sub "An organization has its own workspaces, repositories and apps, and
  people in it as owners, admins or members."
- Empty state "No organizations yet" with "You are not in an organization yet. Create one, or ask a
  colleague to invite you by your email address." and **New organization**.
- The header shows **Join** and **New organization**. It shows **Convert your workspace** only when
  the person has a personal space.

#### ORG-002 · Create the organization TD-ORG · P0
As: owner

1. Open **Organization → Organizations** and press **New organization**.
2. Check the dialog: title "New Organization", subtitle "You become its owner. It gets its own
   workspaces, separate from yours; invite colleagues next."
3. Type `qa-<MMDD>-org` in **Organization name** and `qa-<MMDD>-org` in **Handle (optional)**.
4. Press **Create organization**.

Expect:
- The address becomes `/organization?id=<new id>&tab=invitations` and the **Invitations** tab is
  current.
- The page title is "qa-<MMDD>-org", with the chip `owner` and "Your role here." beside it.
- Back on **Organizations**, a card for the organization shows the `owner` chip, "1 member" and
  "Created" with a relative time.

#### ORG-003 · New organization refuses a bad or taken handle · P2
As: owner

1. Press **New organization**. Leave **Organization name** empty and press **Create
   organization**.
2. Type `qa-<MMDD>-bad` as the name and `Bad_Handle` as the handle; press **Create organization**.
3. Change the handle to `qa-<MMDD>-org` (taken by ORG-002) and press **Create organization**.
4. Press **Cancel**.

Expect:
- Step 1: the browser refuses to submit and marks **Organization name** as required.
- Step 2: the browser refuses the handle as not matching the requested format; nothing is sent.
- Step 3: Lake's refusal (`conflict`) shows in the dialog; no second organization appears.
- Step 4: the dialog closes and nothing was created.

#### ORG-004 · The list's figures, search and role filter · P1
As: owner

1. Open **Organization → Organizations**.
2. Type `qa-<MMDD>` into **Search organizations…**, then `zzz-none`.
3. Clear the search. Press the role chip `owner`, then **Any role**.

Expect:
- Four figures: **Organizations** ("you belong to or can see"), **You own** ("as owner"), **You
  administer** ("as admin") and **Invitations** ("none waiting" when there are none).
- Step 2: the first search keeps `qa-<MMDD>-org`; the second shows "No organization matches" and
  "Clear the search or the role filter to see more."
- Step 3: the chip narrows the cards to organizations you own; **Any role** shows all again.
- Each card's footer has **invite**, **teams**, **members**, **invitations**, **effective access**
  and **SSO**, and each opens its page for this organization.

#### ORG-005 · The operator's Organizations page · P1
As: operator

1. Open **Administration → Organizations**.

Expect:
- No **Join**, **New organization** or **Convert your workspace** buttons and no invitation banner.
- Two figures: **Organizations** (sub "on this Lake") and **Invitations**.
- `qa-<MMDD>-org` is listed, with no role chip.

## Invitations

#### ORG-006 · Invite member by email · P0
As: owner · Needs: TD-ORG

1. Open `TD-ORG` and press **Invite people**.
2. Check the dialog: title "Invite a Member", subtitle "They join from their Organizations page once
   they sign in with that email address, or with the workspace that has that handle."
3. Type `member`'s sign-in email in **Email or handle**. Leave **Type** on **Email**, **Expires in**
   on **7 days** and **Role** on "Member: can read the organization".
4. Press **Send invitation**.

Expect:
- Toast "Invitation sent"; the dialog closes.
- The **Invitations** tab lists the address with sub `email`, the chip `member`, status `pending`, an
  expiry about 7 days away and **Revoke**.
- The tab's count badge and the Overview's **Pending invitations** figure go up by one.
- No email arrives in `member`'s inbox (Lake sends none).

#### ORG-007 · Member accepts the invitation from Organizations · P0
As: member · Needs: ORG-006

1. Sign in as `member` with the invited email and open **Organization → Organizations**.
2. Press **Review** on the banner.
3. In "Invitations for You", press **Join** on `qa-<MMDD>-org`.

Expect:
- Step 1: banner "1 invitation waiting for you" with "qa-<MMDD>-org invited you to join." and
  **Review**; the header button reads **Join (1)**.
- Step 2: the dialog lists **Organization**, **Role** (`member`) and **Expires**.
- Step 3: the button spins, then the browser opens `/organization?id=<TD-ORG id>` with the chip
  `member` and "Your role here."
- As `owner`, the invitation's status is now `accepted` and has no **Revoke**; the **Members** tab
  lists `member`'s TAAP identity with role `member`.

#### ORG-008 · The invite dialog's fields and choices · P2
As: owner · Needs: TD-ORG

1. Press **Invite people** and open each select.
2. Press **Send invitation** with **Email or handle** empty.
3. Press **Escape**.

Expect:
- **Type**: `Email`, `Handle`. **Expires in**: `1 day`, `7 days` (selected), `30 days`.
- **Role**: "Member: can read the organization" (selected), "Admin: can manage people, teams and
  access", "Owner: can manage; an organization always keeps at least one".
- **Email or handle** has focus when the dialog opens.
- Step 2: the browser refuses to submit; nothing is sent.
- Step 3 closes the dialog and nothing was created.

#### ORG-009 · Revoke an invitation: wrong word, then right · P1
As: owner · Needs: TD-ORG

1. Invite `qa-<MMDD>-nobody@<test inbox domain>` as `member` (ORG-006's steps).
2. On its row, press **Revoke**.
3. Type `Revoke` into the confirmation field.
4. Clear it, type `revoke` and press **Revoke invitation**.

Expect:
- Step 2: dialog "Revoke the Invitation to qa-<MMDD>-nobody@…", line "qa-<MMDD>-nobody@… can no
  longer accept the invitation to join as member. It stays listed as revoked.", and the field
  "Type `revoke` to confirm".
- Step 3: **Revoke invitation** stays disabled.
- Step 4: toast "Invitation revoked"; the row stays, with status `revoked` and no **Revoke**.

#### ORG-010 · Search and filter the Invitations tab · P2
As: owner · Needs: ORG-007, ORG-009

1. On `TD-ORG`'s **Invitations** tab, press the status chips `accepted`, `revoked`, then
   **Any status**.
2. Type `nobody` into **Search invitations…**, then `zzz-none`.

Expect:
- Each chip shows its count and narrows the rows to that status.
- `nobody` keeps the revoked row; `zzz-none` shows "No invitation matches" and "Clear the search or
  the status filter to see more."
- **Invitee** and **Expires** sort when their headers are pressed.

#### ORG-011 · No invitation waits for another email · P2
As: newcomer

1. Open **Organization → Organizations** and press **Join**.

Expect:
- "Invitations for You" shows "No invitations waiting" with "Ask a colleague to invite you by your
  email address or your handle."

#### ORG-012 · The Invitations page · P2
As: owner · Needs: TD-ORG

1. On the `TD-ORG` card, press **invitations**.
2. Press **Invite someone**.

Expect:
- Title "Invitations", sub "Email and handle invitations expire and become attested memberships."
- The same rows, chips and **Revoke** as the tab.
- Step 2 opens `/organization?id=<TD-ORG id>&tab=invitations`. The invite dialog does not open by
  itself (suspected defect 4).

## Organization Settings

#### ORG-013 · The Overview tab · P0
As: owner · Needs: TD-ORG

1. Open `TD-ORG` (its card's title) and stay on **Overview**.
2. Press each figure in turn, then come back.

Expect:
- Figures **Members** (sub "1 owner"), **Teams** and **Pending invitations**; each opens its tab.
- Card **Organization** with **Identity**, **ID**, **Teams**, **Members**, **Pending invitations**
  and **Created**.
- Card **Pages** with **Teams**, **Members and their attestations**, **Invitations**, **Effective
  access** and **Enterprise SSO and provisioning log**; each opens its page.
- The tabs are **Overview**, **Members**, **Teams**, **Invitations**, **Access**, **SSO**,
  **Applications**, **Usage** and **Audit**; Members, Teams and Invitations show counts.

#### ORG-014 · Tabs live in the address · P2
As: owner · Needs: TD-ORG

1. Press **Audit**, then reload.
2. Change the address to `tab=nothing`.
3. Open `/organization` with no `id`.

Expect:
- Step 1: the Audit tab is still current after the reload.
- Step 2: the tab bar shows, no tab is current, and the page body below it is empty.
- Step 3: the browser goes to `/organizations`.

## Teams

#### ORG-015 · Create the team qa-team · P0
As: owner · Needs: TD-ORG

1. Open `TD-ORG`'s **Teams** tab.
2. Check the empty state, then press **New team**.
3. Type `qa-team` in **Team name** and `QA Team` in **Display name**; press **Create team**.

Expect:
- Step 2: "No teams yet" with "No teams in this organization yet. A team groups members so access can
  be granted to all of them at once." and **New team**.
- Dialog title "New Team"; **Team name** has the help "Lowercase letters, numbers and inner
  hyphens."
- Step 3: toast "Team created"; the row shows "QA Team" with `qa-team` below it, **Members** `0`,
  **members**, **effective access** and **Created**.

#### ORG-016 · New team refuses a bad name · P2
As: owner · Needs: TD-ORG

1. Press **New team**, type `QA Team` in **Team name** and press **Create team**.
2. Change it to `qa-team` (already taken) and press **Create team**.

Expect:
- Step 1: the browser refuses the name as not matching the requested format.
- Step 2: Lake's refusal shows under **Team name** or above the form; no second team appears.

#### ORG-017 · The Teams tab and the Teams page · P2
As: owner · Needs: ORG-015

1. On the **Teams** tab, type `qa` into **Search teams…**, then `zzz-none`.
2. Press the **Team**, **Members** and **Created** headers.
3. Open the `TD-ORG` card's **teams** link, then press **New team** there.

Expect:
- Step 1: `zzz-none` shows "No team matches" and "Clear the search to see every team."
- Step 2: each header sorts ascending, then descending on a second press.
- Step 3: page "Teams", sub "Roles on an organization flow to its teams unless inheritance is cut.";
  **New team** opens the organization's **Teams** tab, not the dialog.
- `/teams` with no `organization_id` goes to `/organizations`.

#### ORG-018 · Team Settings · P1
As: owner · Needs: ORG-015

1. Press "QA Team" on the Teams tab.

Expect:
- Title "QA Team", sub "Team `qa-team` in qa-<MMDD>-org" with the organization linked.
- **Effective access** in the header opens `/team/access?id=<team id>`.
- Tabs **Overview** (current), **Members** (with a count) and **Access**.
- Card **Members** with **All members →**, search **Search members…** and the empty state "No team
  members" / "Members join a team from the organization's members."
- Card **Team**: **Handle** `qa-team`, **Organization**, **Members**, **Effective access**
  ("<n> access entries.") and **Created**.

## Members

#### ORG-019 · The Members tab · P1
As: owner · Needs: ORG-007

1. Open `TD-ORG`'s **Members** tab.
2. Press the role chip `member`, then **Any role**.
3. Type part of `member`'s TAAP identity into **Search members…**.

Expect:
- Columns **Member** (a TAAP identity linked to `/member?id=`, with the principal type below),
  **Role**, **Scope** ("Organization") and **Attestation**.
- `owner` shows the `owner` chip and `member` the `member` chip.
- Chips and search narrow the rows; a name or email does not match (only the TAAP identity and
  type are searched).

#### ORG-020 · The Members page and a team's members · P2
As: owner · Needs: ORG-015

1. On the `TD-ORG` card, press **members**.
2. On the Teams tab, press **members** on `qa-team`.

Expect:
- Step 1: title "Members", sub "Every role is backed by an org_membership/1 attestation.", columns
  **Member**, **Role**, **Attestation** and **Granted by**.
- **Invite people** opens the organization's **Invitations** tab.
- Step 2: the address carries `team_id`; the empty state reads "No members" with "No members yet.
  Invite people to add them."

#### ORG-021 · Invite writer by handle and join · P1
As: owner, then writer · Needs: TD-ORG

1. As `owner`, press **Invite people**, type `writer`'s handle, set **Type** to **Handle** and send.
2. As `writer`, open **Organization → Organizations**, press **Join (1)**, then **Join**.

Expect:
- Step 1: the invitation row shows sub `handle`.
- Step 2: `writer` lands on `TD-ORG` with the chip `member`.

#### ORG-022 · Member Settings · P1
As: owner · Needs: ORG-021

1. On the **Members** tab, press `writer`'s identity.

Expect:
- Page title "Member" with the TAAP identity and the `member` chip beneath; the tab title is
  "Lake — Member Settings".
- Card **Effective access** titled "<n> entries at this scope." with **Role**, **Actions** (chips
  such as `read`) and **Source** (`direct` or `inherited`).
- Card **Membership**: **Principal**, **Role**, **Scope** (linked "Organization") and **Attestation
  evidence**.
- Card **Change role** with a select on `member` and **Change role** disabled.
- Card **Remove** with "Revokes the membership and the grants derived from it." and **Remove
  member**.

#### ORG-023 · Change a member's role · P1
As: owner · Needs: ORG-022

1. On `writer`'s member page, choose `admin` and press **Change role**.
2. Choose `member` again and press **Change role**.

Expect:
- **Change role** is enabled only while the select differs from the current role.
- Step 1: toast "Role changed"; the role chip becomes `admin`. As `writer`, `TD-ORG` now shows the
  chip `admin`.
- Step 2: toast "Role changed"; the chip is `member` again.

#### ORG-024 · The last owner cannot be demoted or removed · P1
As: owner · Needs: TD-ORG

1. Open your own membership from the **Members** tab.
2. Choose `member` and press **Change role**.
3. Press **Remove member**, then **Confirm removal**.

Expect:
- Step 2: Lake's refusal shows under the form, in red; the role stays `owner`.
- Step 3: the refusal shows inside the dialog; the membership stays.

#### ORG-025 · Remove a member, opened from the address · P1
As: owner · Needs: ORG-021

1. Open `/member?id=<writer's membership id>&confirm_removal=yes`.
2. Press **Cancel**, then **Remove member** again.
3. Press **Confirm removal**.

Expect:
- Step 1: the dialog "Remove the member?" is open at once, with the warning "Removing this member
  revokes their membership and derived grants immediately." and **Cancel**, **Confirm removal**.
- There is no typed confirmation in this dialog.
- Step 3: the browser goes to `/organizations` (no toast). `writer` is gone from the **Members** tab,
  and as `writer`, `TD-ORG` is no longer listed.

#### ORG-026 · What a plain member sees · P1
As: member · Needs: ORG-007

1. Open `TD-ORG`.
2. Open **Invitations**, then press **Invite people** and send an invitation to
   `qa-<MMDD>-nobody@<test inbox domain>`.
3. Open **Teams**, press **New team** and create `qa-member-team`.

Expect:
- No **Delete organization** button; **Invite people** is shown.
- Overview: **Pending invitations** reads "—" with sub "admins only".
- Step 2: the tab shows "You don't have access to this" with Lake's message; the invite is refused
  inside the dialog. No invitation is created.
- Step 3: the create is refused inside the dialog; no team is created.

## Access and grants

#### ORG-027 · The Access tab · P1
As: owner · Needs: ORG-007

1. Open `TD-ORG`'s **Access** tab.
2. Type part of `member`'s identity into **Search principals…**.
3. Press **Open effective access**.

Expect:
- Columns **Principal**, **Role** and **Source**; `owner` and `member` each have a row.
- Step 2 narrows the rows; a search with no match shows "No effective access entries" / "Nobody
  reaches this organization yet."
- Step 3 opens `/organization/access?id=<TD-ORG id>`.

#### ORG-028 · Effective Access for the organization · P1
As: owner · Needs: ORG-007

1. Open `/organization/access?id=<TD-ORG id>`.
2. Press the role chip `read`, then **Any role**.

Expect:
- Title "Effective Access", sub "Who can access `organization` <external id>, what they can do, and
  where it comes from."
- Figures **Principals**, **Direct** ("granted on this object") and **Inherited** ("from the scopes
  above it").
- Columns **Principal**, **Role**, **Actions** (chips such as `read`, `admin`) and **Source**
  ("direct on" or "inherited from" and a linked object id).
- **Direct grants** in the header opens `/grants?object_id=<object id>`.

#### ORG-029 · A team inherits the organization's roles · P2
As: owner · Needs: ORG-015

1. Open `/team/access?id=<qa-team id>`.

Expect:
- `owner` and `member` appear with Source "inherited from" and the organization's object id.
- **Inherited** is greater than 0.

#### ORG-030 · A repository's effective access · P2
As: owner · Needs: TD-REPO

1. Open `/repository/access?id=<TD-REPO id>`.

Expect:
- Sub names `repository` and the repository's id.
- `member`'s `read` grant shows as "direct on"; `owner` shows its account role.

#### ORG-031 · Direct Grants · P1
As: owner · Needs: ORG-028

1. On Effective Access, press a source id in the **Source** column.

Expect:
- Title "Direct Grants", sub "Attestation-backed roles attached directly to `<type>` <external
  id>."
- Columns **Principal**, **Role**, **Attestation**, **Granted by**, and **Revoke** per row.
- **View effective access** returns to `/access?object_id=<id>`.
- Search **Search principals…** and the role chips narrow the rows.

#### ORG-032 · Revoking a membership's grant is refused · P1
As: owner · Needs: ORG-031

1. On `TD-ORG`'s Direct Grants, press **Revoke** on `member`'s grant.
2. Type `REVOKE`; then clear it, type `revoke` and press **Revoke grant**.

Expect:
- Dialog title "Revoke read Access" (the grant's role), intro "A grant that comes with a membership
  is removed with the member, and the last admin grant on an account is kept, so Lake may refuse.",
  line "The read role of this principal on this object. Access it has from elsewhere stays."
- `REVOKE` keeps **Revoke grant** disabled.
- After `revoke`, Lake's refusal stays in the dialog; the grant is still listed after **Cancel**.

#### ORG-033 · Access pages without an object · P2
As: owner

1. Open `/grants`, `/access`, `/organization/access` and `/team/access`, each with no query.
2. Open `/grants?object_id=no-such-object`.

Expect:
- Step 1: each goes to `/organizations`.
- Step 2: an error block (403 or 404 title) with Lake's message; no table.

## Enterprise SSO

#### ORG-034 · The SSO tab · P1
As: owner · Needs: TD-ORG

1. Open `TD-ORG`'s **SSO** tab.

Expect:
- "Provider secrets and SCIM tokens are never shown." with **Provisioning log** and **Add
  provider**.
- Empty state "No SSO providers" with "Connect an OpenID Connect or SAML identity provider so
  members sign in with it." and **Add provider**.

#### ORG-035 · Add provider is refused for missing protocol settings · P1
As: owner · Needs: TD-ORG

1. Press **Add provider**.
2. Check the drawer: title "Configure SSO Provider"; fields **Name**, **Protocol**, **Issuer**,
   **SCIM token**.
3. Type `qa-<MMDD>-oidc`, keep **OpenID Connect**, type `https://accounts.google.com` and a long
   random SCIM token; press **Save provider**.
4. Change **Protocol** to **SAML 2.0** and press **Save provider** again.

Expect:
- **Protocol** offers `OpenID Connect` and `SAML 2.0`. There are no client id, client secret,
  metadata or certificate fields.
- Steps 3 and 4: Lake refuses with "provider protocol configuration is incomplete" (or its JSON
  pointer's field); the drawer stays open; no provider is listed. This is the expected behaviour
  today (`docs/guide/sso.md`, Limits; suspected defect 6).
- Pressing **Save provider** with **Name** empty is blocked by the browser.

#### ORG-036 · A provider registered through the API is listed · P2
As: owner · Needs: TD-ORG, an API token

1. Register an OIDC provider with `POST /v1/register_sso_provider` (`protocol: "oidc"`, issuer
   `https://accounts.google.com`, a `client_id`, a `client_secret`, a SCIM token,
   `jit_provisioning: true`).
2. Open the **SSO** tab.

Expect:
- The row shows the name linked to the Enterprise SSO page, the issuer below it, Protocol `OIDC`
  and Tested "not tested" (amber).
- No secret or SCIM token appears anywhere on the page or in the `list_sso_providers` answer.

#### ORG-037 · The Enterprise SSO page · P2
As: owner · Needs: ORG-036

1. Press **Provisioning log** on the SSO tab.
2. Press the provider's name.

Expect:
- Title "Enterprise SSO", sub "Provider configuration, federation policy, and SCIM provisioning
  evidence.", and the banner "Add a provider from the organization's settings. Provider secrets
  and SCIM tokens are never shown here."
- Figures **Providers** ("0 tested"), **Issuer claims** ("allowed for federation") and
  **Provisioning events** ("for <provider name>").
- Card **Providers** with **Provider**, **Protocol**, **JIT** (`enabled`) and **Tested**; the
  chosen row has a left accent bar.
- Card **Allowed issuer claims** reads "No issuer claims are allowed yet."
- Card **Provisioning log** reads "No SCIM provisioning events for this provider."
- **Add a provider** opens the organization's **SSO** tab; the drawer does not open by itself.
- `/sso` with no `organization_id` goes to `/organizations`.

#### ORG-038 · Lake's SAML metadata opens in the browser · v0.4.0+ · P2
As: owner · Needs: TD-ORG, an API token

1. Register a SAML provider with `register_sso_provider` (`protocol: "saml"`, `metadata`,
   `signing_certificate`).
2. Open `https://<host>/v1/saml/<provider id>/metadata` in a tab.
3. Open the **SSO** tab.

Expect:
- Step 2 answers XML (`application/samlmetadata+xml`) whose entity id is that same URL.
- Step 3 lists the provider with Protocol `SAML`.

## Applications, Usage and Audit tabs

#### ORG-039 · The Applications tab · P2
As: owner · Needs: TD-ORG

1. Open `TD-ORG`'s **Applications** tab.

Expect:
- With none: "No applications" / "No applications belong to this organization customer." and
  **Applications** (opens `/applications`).

#### ORG-040 · The Usage tab · P2
As: owner · Needs: TD-ORGWS

1. Open `TD-ORG`'s **Usage** tab.
2. Press **Open usage**.

Expect:
- Figures **Stored** ("repositories, images and workspace data"), **Repository storage limit**
  ("No limit" with "writes are never refused for size", or a size and a meter) and **Compute**
  ("builds and workspace runs").
- Step 2 opens `/metering?customer_id=<TD-ORG's account id>`.
- On an organization with no workspace account the tab reads "No workspace" / "No workspace is
  attached to this organization."

#### ORG-041 · The Audit tab and its export · P1
As: owner · Needs: TD-ORG

1. Open `TD-ORG`'s **Audit** tab.
2. Press an outcome chip, then **Any outcome**. Type `invitation` into **Search actions…**.
3. Press **Export**.

Expect:
- Columns **When**, **Action** (with the capability below), **By**, **Object** (chip), **Outcome**
  and **Log** (`Central` or a workspace id).
- The invitations, team and role changes made in this file are listed.
- Step 2 narrows the rows; no match shows "No matching audit events" / "Changes to this
  organization and its workspaces are recorded here."
- Step 3 downloads or opens JSON lines from `/v1/export_audit?limit=1000&customer_id=…`
  (suspected defect 7: the guide says only the operator may export).

#### ORG-042 · A member's Audit tab · P2
As: member · Needs: ORG-007

1. Open `TD-ORG`'s **Audit** tab.

Expect:
- The table area shows the refusal ("You don't have access to this" with Lake's message); the tab
  bar and the rest of the page still work.

## Deleting an organization

#### ORG-043 · Delete is blocked while the organization owns a workspace · P1
As: owner · Needs: TD-ORGWS

1. Open `TD-ORG` and press **Delete organization**.
2. Press the workspace link in the warning, then come back without deleting anything.
3. Press **Cancel**.

Expect:
- Dialog "Delete qa-<MMDD>-org", intro "Deleting the organization removes everything below. It
  cannot be undone."
- Lines: "· The organization itself. Its handle is retired and cannot be used again.", then
  "2 members, who lose access" and the non-zero counts of teams, invitations, access grants and SSO
  providers.
- A warning "Delete its workspace first" / "The organization still owns this workspace; each holds
  data that is deleted with it." with the workspace's name linked to `/workspaces?delete=<id>`.
- There is no typed field, and **Delete organization** is disabled.
- Step 2 opens Workspaces with that workspace's deletion asked; nothing is deleted.

#### ORG-044 · Delete an organization: wrong handle, then right · P1
As: owner

1. Create `qa-<MMDD>-org2` (handle `qa-<MMDD>-org2`) as in ORG-002.
2. Press **Delete organization**.
3. Type `QA-<MMDD>-ORG2`, then `qa-<MMDD>-org`.
4. Type `qa-<MMDD>-org2` and press **Enter**.

Expect:
- The field is "Type `qa-<MMDD>-org2` to confirm"; the line "1 member, who lose access" is shown.
- Step 3: **Delete organization** stays disabled for both.
- Step 4: Enter submits; toast "Deleted qa-<MMDD>-org2"; the browser is on `/organizations` and the
  card is gone.
- Creating a new organization with handle `qa-<MMDD>-org2` is refused (the handle is retired).

#### ORG-045 · Only an owner sees Delete organization · P2
As: member · Needs: ORG-007

1. Open `TD-ORG`.

Expect:
- The header has **Invite people** only; no **Delete organization**.

## Turning a workspace into an organization

#### ORG-046 · A wrong handle changes nothing · P1
As: owner

1. On **Organizations**, press **Convert your workspace**.
2. Type `qa-<MMDD>-convert` in **Organization name** and `wrong-handle` in the confirm field; press
   **Turn into an organization**.
3. Press **Cancel**.

Expect:
- Title "Turn Your Workspace into an Organization"; the body names your handle and says "Lake gives
  you a new, empty workspace with a generated handle."; warning "This cannot be undone here."
- The confirm field is labelled "Type `<your handle>` to confirm"; the action is a red **Turn into
  an organization**.
- Step 2: under the confirm field, "type the handle <your handle> exactly to confirm turning this
  workspace into an organization; nothing was changed".
- Your handle, workspaces and organizations are unchanged.

#### ORG-047 · Convert a workspace for real · P2
As: spare (a throwaway persona with a personal workspace; skip when there is none)

1. Press **Convert your workspace**, type `qa-<MMDD>-converted` and the exact handle; press **Turn
   into an organization**.

Expect:
- The browser opens `/organization?id=<new id>&tab=invitations`, with `spare` as `owner`.
- The old handle's repositories and workspaces now belong to the organization; `spare` has a new
  personal space with a generated handle.
- Delete the organization's workspaces and the organization at clean-up.

## Secrets (account holder)

#### ORG-048 · Secret Mounts with no workspace · P2
As: newcomer

1. Open **Organization → Secrets**.

Expect:
- Title "Secret Mounts", sub "Static secrets and named encryption keys, kept in the workspace each
  mount was created in. Your account or one of your organizations owns each mount. Key material
  remains inside Lake."
- "Choose a workspace" / "Choose a workspace to see its secret mounts."; **New mount** is disabled.

#### ORG-049 · Create the mount TD-SECRET · P0
As: owner · Needs: W1

1. Open **Organization → Secrets**; choose `W1` in **Workspace** if asked.
2. Press **New mount**.
3. Keep **Workspace** `W1` and **Owner** "Your account"; type `qa-<MMDD>-mount` and the description
   `qa run`; press **Create mount**.

Expect:
- Dialog "New Secret Mount", subtitle "A mount holds secrets and encryption keys in one workspace,
  for your account or one of your organizations."
- **Workspace** groups workspaces by account and has the help "Its secrets and keys are sealed in
  this workspace. It cannot be changed later."
- **Create mount** is disabled until a name is typed.
- Toast "Created mount qa-<MMDD>-mount"; the browser opens `/secrets?mount_id=<id>` titled
  "qa-<MMDD>-mount" with sub "qa run".

#### ORG-050 · New mount's owner choices and name rule · P1
As: owner · Needs: TD-ORGWS

1. Press **New mount**. Choose `TD-ORGWS` in **Workspace** and open **Owner**.
2. Choose `W1` again and open **Owner**.
3. Type `Bad Name` and press **Create mount**; then type `qa-<MMDD>-mount` (taken in `W1`) and press
   it again.
4. Press **Cancel**.

Expect:
- Step 1: the first option is "qa-<MMDD>-org (workspace owner)"; "Your account" is not offered.
- Step 2: the first option is "Your account" (selected), then `qa-<MMDD>-org` if you administer it
  and its own space is yours; changing the workspace reset the owner.
- Step 3: the browser refuses `Bad Name`; the taken name shows Lake's refusal under **Name** or
  above the form.
- Nothing is created.

#### ORG-051 · The mounts list · P1
As: owner · Needs: TD-SECRET, TD-W2

1. Open **Organization → Secrets** with `W1` chosen.
2. Choose `W2` in **Workspace**, then `W1` again.
3. Type `qa` into **Search mounts…**, then `zzz-none`.
4. Open `/secret-mounts?workspace_id=not-mine`.

Expect:
- Figures **Mounts** ("in W1") and **Owners** ("own these mounts").
- Columns **Mount** (linked, description below), **Owner** ("Your account"), **Workspace**,
  **Created**, and the buttons **Secrets**, **Keys** and **Delete**.
- Step 2: the address carries `workspace_id`; `W2` shows "No secret mounts" with "No secret mounts
  you can read in W2 yet. A mount holds secrets and keys for your account or one of your
  organizations." and **New mount**.
- Step 3: `zzz-none` shows "No mount matches" / "Clear the search to see every mount."
- Step 4: an error block "workspace_id must name one of your workspaces".

#### ORG-052 · Write the secret crm · P0
As: owner · Needs: TD-SECRET

1. Open `qa-<MMDD>-mount` and check the empty state.
2. Press **New secret**. Type `crm` as **Path**.
3. Under **Key and value**, type `token` and a test value; press **Save secret**.

Expect:
- Step 1: "No secrets yet" with "Write the first secret: a path such as `database/password` and a
  JSON value." and **New secret**.
- Drawer "New Secret", subtitle "A path and a JSON object, written as version 1."; the value field
  is a password field; the help reads "One JSON object. Values are written once and never shown
  here again."
- Toast "Saved crm as version 1"; the browser opens `/secrets?mount_id=…&name=crm`, titled `crm`
  with a `live` chip. The value appears nowhere on the page or in the address.

#### ORG-053 · The value editor refuses bad values · P1
As: owner · Needs: TD-SECRET

1. Press **New secret**, path `qa-bad`.
2. Leave every row empty and press **Save secret**.
3. Type a value with no key; press **Save secret**.
4. Press **Add a field**; make both rows key `a`; press **Save secret**.
5. Switch to **JSON**, type `[1,2]`, then `{bad`, then clear it; press **Save secret** each time.
6. Open **Custom metadata**, type `x` there with a valid JSON value; press **Save secret**.

Expect:
- Step 2: "Add at least one key and value."
- Step 3: "Row 1 has a value but no key." and row 1 is outlined in red.
- Step 4: "The key "a" is used twice."
- Step 5: "The value must be a JSON object, in braces.", then "The value is not valid JSON: …",
  then "The value is empty: write a JSON object such as {"password": "…"}."
- Step 6: a message under **Custom metadata (JSON)** starting "Custom metadata is not valid JSON".
- Nothing is sent: no secret `qa-bad` is listed.
- **Save secret** is disabled while **Path** is empty.

#### ORG-054 · Switching between rows and JSON · P2
As: owner · Needs: TD-SECRET

1. In **New secret**, add rows `user` = `u` and `pass` = `p`, then switch to **JSON**.
2. Replace the JSON with `{"port": 5432}` and switch to **Key and value**.
3. Press **Cancel**.

Expect:
- Step 1: the JSON editor shows both fields as an object.
- Step 2: the note "Some fields are not text (numbers, objects), so they stay in the JSON editor."
  and the editor stays on JSON.
- Step 3: nothing is saved; reopening the drawer starts empty.

#### ORG-055 · A listed path becomes its next version · P2
As: owner · Needs: ORG-052

1. On the mount page, press **New secret** and type `crm` as **Path**.

Expect:
- An info banner "`crm` already exists. Saving adds version 2, checked against version 1."
- Saving gives "Saved crm as version 2". (Press **Cancel** instead to keep the counts for later
  cases at version 1.)

#### ORG-056 · Reveal, show, copy and hide a value · P1
As: owner · Needs: ORG-052

1. On `crm`'s page, press **Reveal value**.
2. Press **Show**, then **Mask**. Press the copy icon beside `token`, then **Copy JSON**.
3. Press **Hide**. Press **Reveal** on version 1 in **Version history**, then leave the page and come
   back.

Expect:
- Step 1: a card "Version 1" / "Value" with the field `token` shown as "••••••••••••"; the header
  button reads **Hide value**.
- Step 2: **Show** shows the value; the copy icon turns into a check and its name becomes "Copied"
  for a moment; **Copy JSON** copies the whole object.
- Step 3: the card disappears on **Hide**; after leaving and coming back, no value is shown.
- The address and the Network panel's `read_secret` request carry no value in the URL.

#### ORG-057 · A new version, and a write that lost the race · P1
As: owner, in two tabs · Needs: ORG-052

1. In tab A, open `crm` and press **New version**. In tab B, do the same.
2. In tab B, set `token` to a new value and press **Save version**.
3. In tab A, set `token` and press **Save version**.

Expect:
- Drawer title "New Version" with `crm` as subtitle, and the metadata help "Leave empty to keep the
  secret's metadata; an object replaces it."
- Step 2: toast "Saved crm as version 2"; **Current version** reads 2.
- Step 3: banner "Someone wrote a newer version" / "Version 1 is no longer current, so nothing was
  written. Close this, look at the new version, and write again."; no version 3 exists.

#### ORG-058 · Delete and undelete a version · P1
As: owner · Needs: ORG-057

1. In **Version history**, press **Delete** on version 2.
2. Press **Delete version**.
3. Press **Undelete** on version 2.

Expect:
- Dialog "Delete Version 2?" with "Version 2 of `crm` becomes unreadable. Its data is kept, and you
  can undelete it later."
- Step 2: toast "Deleted version 2"; version 2's state is `deleted`; the title chip reads `deleted`;
  banner "The current version is deleted" / "Reading this secret fails until a version is
  undeleted or written again."; **Reveal value** is gone.
- Step 3 runs at once: toast "Undeleted version 2"; state `live` again.

#### ORG-059 · Destroy a version: wrong path, then right · P1
As: owner · Needs: TD-SECRET

1. Create the secret `qa-destroy` with one field.
2. On its page, press **Destroy** on version 1.
3. Type `QA-DESTROY`, then `qa-destroy` and press **Destroy for good**.

Expect:
- Dialog "Destroy Version 1?" with "The value of version 1 of `qa-destroy` is erased forever. It
  can never be read or undeleted again; only its history row stays." and "Type `qa-destroy` to
  confirm".
- `QA-DESTROY` keeps **Destroy for good** disabled.
- Toast "Destroyed version 1"; the row reads "erased" with no actions; **Destroyed** is 1 (red);
  banner "The current version is destroyed" / "Reading this secret fails until a version is written
  again."

#### ORG-060 · The mount's secrets list · P1
As: owner · Needs: ORG-059

1. Open `qa-<MMDD>-mount`.
2. Press the chip **Deleted**, then **Live**, then **All**.
3. Type `crm` into **Search paths…**.

Expect:
- Figures **Secrets** ("in this mount"), **Live** ("current version readable") and **Deleted**
  ("current version deleted or destroyed").
- Columns **Secret** (path, "version <n>" below), **State**, **Version**, **Updated**, **Created**;
  `qa-destroy` reads `destroyed`.
- Chips narrow to destroyed or live rows; the search keeps `crm` only.
- Side card **Mount**: **Description**, **Workspace**, **Owner** ("Your account"), **Created**,
  **Mount id**, and links **Encryption keys** and **All secret mounts**.
- No Previous or Next buttons with fewer than 201 secrets.

#### ORG-061 · Paging past 200 secrets · P2
As: owner · Needs: TD-SECRET, 201 secrets written with `put_secret` through the API

1. Open the mount. Press **Next**, then **Previous**.

Expect:
- **Secrets** reads "200+" with "on this page".
- **Previous** is disabled on the first page; **Next** is disabled on the last.

#### ORG-062 · A member's view of Secrets · P2
As: member

1. Open **Organization → Secrets** on `W1`.
2. Press **New mount** and try to create `qa-<MMDD>-member-mount` in `W1`.

Expect:
- `qa-<MMDD>-mount` is not listed unless `member` was granted on it.
- Step 2 is refused inside the dialog with Lake's message; nothing is created.

#### ORG-063 · Delete a mount: wrong name, then right · P1
As: owner · Needs: W1

1. Create `qa-<MMDD>-mount2` with one secret `a`.
2. On **Secret Mounts**, press **Delete** on it.
3. Open **Every secret by name**.
4. Type `qa-<MMDD>-mount`, then `qa-<MMDD>-mount2` and press **Delete mount**.

Expect:
- Dialog "Delete qa-<MMDD>-mount2", intro "Deleting the mount removes everything in it from its
  workspace. It cannot be undone."
- Lines "1 secret with 1 version" and "· Every access grant on the mount and its secrets."
- Step 3 lists `a`.
- The wrong name keeps **Delete mount** disabled.
- Toast "Deleted qa-<MMDD>-mount2"; the row is gone. `TD-SECRET` is untouched.

#### ORG-064 · New Secret Mount on a phone · P2
As: owner · Needs: W1

1. At 390×844, open **Organization → Secrets** and press **New mount**.
2. Tap **Name** so the keyboard opens.

Expect:
- The dialog is a full-screen sheet; **Cancel** and **Create mount** are pinned at its bottom, above
  the keyboard.

## Your Activity

#### ORG-065 · Your Activity lists your events · P0
As: owner

1. Open **Organization → Activity**.

Expect:
- Title "Your Activity", sub "Signed, hash-linked events for everything you did on this Lake."
- Figures **Events** ("newest first"), **Succeeded**, **Denied** and **Failed**.
- The quiet banner "Successful reads, such as the polling this page and the workspace views do, are
  hidden. Failed and denied reads are shown."
- Columns **When** (a coloured dot and a relative time), **Action**, **Principal**, **Object**,
  **Outcome**, **Log** (`Central` or "Workspace <name>") and **Digest** (or "being written").
- This file's writes (organization, invitations, mount, secret) are listed.
- The footer reads "API: `GET /v1/audit`, `GET /v1/export_audit`, and `GET /v1/verify_audit`."

#### ORG-066 · Outcome chips, reads and order · P1
As: owner, then member

1. As `member`, try **Invite people** on `TD-ORG` again (refused).
2. As `owner`, on Your Activity, press **Denied**, then **Succeeded**, then **Any outcome**.
3. Press **Include reads**.
4. Press **Newest first**.

Expect:
- Step 1: as `member`, Your Activity shows the refused invitation as `denied`.
- Step 2: each chip sets `?outcome=` and the list holds only that outcome.
- Step 3: `?reads=all`, the quiet banner is gone, successful reads appear.
- Step 4: the button reads **Oldest first**, `?order=oldest`, the oldest event is on top and the
  **Events** sub reads "oldest first".
- At 500 events the figure reads "500+".

#### ORG-067 · The Filters form · P2
As: owner

1. Press **Filters**.
2. Type `repository/` in **Object**, press **Filter**, then clear it.
3. Type a past date such as `2026-10-01T00:00:00Z` in **From** and press **Filter**.
4. Type `yesterday` in **To** and press **Filter**.

Expect:
- Fields **Object** (placeholder `repository/…`), **Outcome**, **From**, **To**, **Order** (`Newest
  first`, `Oldest first`) and the checkbox **Include reads**; there is no **Principal** field.
- Step 2: the address carries `object=`; **Filters** is highlighted and stays open after a reload.
- Step 3 narrows to events from that time.
- Step 4: the page gives no hint before sending. Lake either refuses it, shown as an error block
  with its message, or ignores it; record which (suspected defect 16).
- Search **Search these events…** narrows the rows read; no match reads "No event matches the
  search."

#### ORG-068 · The operator has no Activity page · P2
As: operator

1. Open `/activity`.

Expect:
- "Not Available" with "This page belongs to an account; the operator credential has no account."
  and **Home**.

## Phone and keyboard

#### ORG-069 · Organization pages on a phone · P2
As: owner · Needs: TD-ORG

1. At 390×844, open **Organizations**, then `TD-ORG`'s **Members**, **Invitations** and **SSO**
   tabs, then a member page.

Expect:
- Cards are one per row; the header buttons wrap under the title.
- The tab bar fits or scrolls inside itself; tables scroll in their own box.
- On the member page, the side cards (Membership, Change role, Remove) drop below Effective access.

#### ORG-070 · Typed confirmation by keyboard · P2
As: owner · Needs: TD-ORG

1. On the **Invitations** tab, invite `qa-<MMDD>-nobody2@<test inbox domain>`, then Tab to its
   **Revoke** and press **Enter**.
2. Type `revok` and press **Enter**; then type `e` and press **Enter**.

Expect:
- Focus moves into the dialog when it opens.
- `revok` + Enter does nothing; `revoke` + Enter revokes ("Invitation revoked") and focus returns to
  the page.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `pages/organizations/MemberPage.tsx:27,34,43` | The page title is "Member" while the registry and tab title say "Member Settings". | ORG-022 |
| 2 | `pages/organizations/MemberPage.tsx:199`; `docs/guide/organizations.md` (Remove a Member) | The guide says confirm with **Remove member**; the button is **Confirm removal**. Removal is the only delete without a typed confirmation, shows no toast and always goes to `/organizations`. | ORG-025 |
| 3 | `pages/organizations/OrganizationPage.tsx:356`; `docs/guide/organizations.md` (Invite People) | The guide names **Invite someone** on an empty Members tab; the button is **Invite people**. | ORG-020 |
| 4 | `MembersPage.tsx:26`, `InvitationsPage.tsx:27`, `TeamsPage.tsx:24`, `SsoPage.tsx:31`, `OrganizationsPage.tsx:140` | **Invite people**, **Invite someone**, **New team**, **Add a provider** and the card's **invite** open the organization's tab but not the dialog or drawer they name. | ORG-012, ORG-017, ORG-037 |
| 5 | `pages/organizations/OrganizationPage.tsx:497-503`; `docs/ui/organization.md` | The UI doc says the invite lasts "1 to 90 days"; the select offers 1, 7 and 30 days only. | ORG-008 |
| 6 | `pages/organizations/OrganizationPage.tsx:593-645` | The SSO drawer has no OIDC client or SAML metadata fields, so every save is refused, with no hint in the drawer. No UI to test a provider, set JIT, or allow issuer claims. | ORG-035 |
| 7 | `pages/organizations/OrganizationPage.tsx:728`; `docs/guide/activity.md` vs `docs/api/audit.md:40-44` | The Audit tab offers **Export** to organization admins; the activity guide says `export_audit` is operator-only, the API doc says admins may export. | ORG-041 |
| 8 | `pages/organizations/OrganizationPage.tsx:128-130`, `:236`, `:556`; `docs/guide/access.md` | **Invite people**, **New team** and **Add provider** show to plain members and are refused by Lake, though the guide says buttons show only when the action would work. | ORG-026 |
| 9 | `pages/organizations/OrganizationPage.tsx:649-653` | The Applications, Usage and Audit tabs find the organization's account by scanning `list_customers` (500 at most) instead of `get_organization`'s `customer_id`; past 500 customers they wrongly say "No workspace" or "No account". | ORG-040 |
| 10 | `pages/organizations/OrganizationPage.tsx:340,371` | Members show only as TAAP identities; search cannot find a person by name or email. | ORG-019 |
| 11 | `docs/guide/access.md` (See Who Can Reach Something) vs `pages/organizations/TeamPage.tsx:77` | The guide says Team Settings lists the effective access; the page shows only a count. | ORG-018 |
| 12 | `lib/deletionDialog.tsx:132` | **Revoke invitation** and **Revoke grant** carry a trash icon. | ORG-009 |
| 13 | `pages/admin/SecretsPage.tsx:400-412` | **Delete version**'s confirm button is not red (ghost trigger without `danger`). | ORG-058 |
| 14 | `pages/admin/SecretsPage.tsx:418-432`; `docs/ui/deletion.md` | Destroy uses its own typed confirm with no dry run, not the one shared deletion dialog the deletion spec describes. | ORG-059 |
| 15 | `pages/admin/SecretMountsPage.tsx:58` | The page is titled "Secret Mounts" while the rail and registry say "Secrets". | ORG-048 |
| 16 | `pages/admin/AuditPage.tsx:156-163` | **From** and **To** are free text with no format hint or check; a bad value fails only at Lake. | ORG-067 |
