# Organizations, Members and Teams

An **organization** is how a group of people shares Lake. It has its own workspaces, repositories
and apps, kept apart from yours, and people in it as owners, admins or members. Teams group people
inside an organization so you can give a set of them access at once.

This page covers creating an organization, inviting and removing people, roles, teams, and turning
your own workspaces into an organization.

## Concepts

| Term | What it is |
|---|---|
| **Organization** | A named group of people with its own workspaces, repositories and apps. Its own account has an id, the `customer_id` that every call about its workspaces names. |
| **Membership** | One person (or workload) in an organization or one of its teams, with a role. Every membership keeps the digest of the attestation Lake signed for it (`lake.org_membership/1`) and the identity of the administrator who granted it. |
| **Role** | `owner`, `admin` or `member`. See [Roles](#roles). |
| **Team** | A named group inside an organization. A membership can be in the organization itself or in one team. |
| **Invitation** | An offer of a membership, addressed to an email address or to a handle, with a role and an expiry. It becomes a membership only when the person joins. |
| **Handle** | The short, lowercase name of an account, as in `handle/repository`. Every organization and every person's own account has one. |

## Roles

| Role | What it can do | What it gives on the organization's workspaces |
|---|---|---|
| **Member** | Read the organization: its teams and its members. | Read access to the organization's workspaces and repositories. |
| **Admin** | Everything a member can, and manage people, teams, invitations, grants and SSO. | Administration of the organization's workspaces and repositories. |
| **Owner** | Everything an admin can. An organization always keeps at least one owner. | The same as an admin. |

How this works: an `owner` or `admin` membership creates an `admin` grant on the organization, and a
`member` membership creates a `read` grant. A membership in the organization itself also gives the
same grant on the organization's own account, so owners and admins administer its workspaces and
members read them. A team membership reaches the team, not the organization's account. Details of
grants and inheritance are on [Grants and Effective Access](/docs/access).

An organization administrator can act on teams, members, invitations and grants **below** that
organization only. They cannot act on a sibling organization.

## Find Your Organizations

Open **Organization → Organizations**. You see one card per organization you belong to or can see,
with your role, its member count and links to its invite, teams, members, invitations, effective
access and SSO pages. The figures at the top count your organizations, the ones you own and
administer, and the invitations waiting for you. Search and the role filter narrow the cards.

- REST: `GET /v1/list_organizations?limit=100`.
- MCP: `list_organizations` with the same input.

`GET /v1/list_memberships` with no input lists your own memberships, in every organization and team.

## Create an Organization

1. Open **Organization → Organizations** and press **New organization**.
2. Enter the **Organization name**, for example *Acme Research*.
3. Optionally enter a **Handle (optional)**: lowercase letters, numbers and inner hyphens, such as
   `acme-research`. Leave it empty and Lake generates one.
4. Press **Create organization**.

You become its owner, the organization gets its own account (separate from yours), and the page
opens the organization's **Invitations** tab so you can invite colleagues next.

- REST: `POST /v1/create_organization_space` with
  `{"display_name": "Acme Research", "name": "acme-research"}`.
- MCP: `create_organization_space` with the same input.

The answer carries the new `organization`, its account (`customer`) and your `membership`. Next,
create a workspace for it on **Ship → Workspaces** ([Workspaces](/docs/workspaces)).

## Turn Your Workspace into an Organization

If you started Lake on your own and now want colleagues in, you can turn your own account (its
handle, repositories, workspaces and apps) into an organization's, without moving anything.

1. Open **Organization → Organizations** and press **Convert your workspace**.
2. Read what happens: the handle, repositories, workspaces and apps stay where they are, so links
   and clones keep working; you become the organization's owner; Lake gives you a new, empty
   account with a generated handle.
3. Enter the **Organization name**.
4. Type your current handle exactly where the dialog asks, then press **Turn into an
   organization**. A wrong handle changes nothing.

This cannot be undone from the page.

- REST: `POST /v1/convert_personal_space` with
  `{"customer_id": "<customer id>", "display_name": "Acme Research"}`, where `<customer id>` is the
  id of your own account.
- MCP: `convert_personal_space` with the same input.

The typed-handle check is the page's. A REST or MCP caller should read the handle first with
`GET /v1/get_customer?customer_id=<customer id>` and compare it.

## Open an Organization

Select an organization's name on **Organization → Organizations**. The organization page has tabs,
kept in the address (`/organization?id=<organization id>&tab=members`):

| Tab | What it shows |
|---|---|
| **Overview** | Figures and facts, and links to the full Teams, Members, Invitations, Effective access and SSO pages. |
| **Members** | Every membership with its role, scope (the organization or a team) and attestation digest. |
| **Teams** | The organization's teams, with **New team**. |
| **Invitations** | Sent invitations with their status and expiry. |
| **Access** | Who reaches the organization, directly or inherited ([Grants and Effective Access](/docs/access)). |
| **SSO** | SSO providers, with **Add provider** and **Provisioning log** ([Single Sign-On and SCIM](/docs/sso)). |
| **Applications** | Identity applications that belong to the organization's account. |
| **Usage** | Storage and compute of the organization's account, with **Open usage** ([Usage and Metering](/docs/usage)). |
| **Audit** | Events about the organization from the central log ([Activity and Audit](/docs/activity)). |

- REST: `GET /v1/get_organization?id=<organization id>`. The answer names the organization's own
  account as `customer_id` (null when it has none).
- MCP: `get_organization` with the same input.

## Invite People

You need to be an admin or owner of the organization.

1. Open the organization and press **Invite people** (or **Invite someone** on an empty Members
   tab).
2. In **Invite a Member**, enter the **Email or handle** of the person.
3. Choose the **Type**: **Email** or **Handle**.
4. Choose how long it lasts in **Expires in**: **1 day**, **7 days** (the default) or **30 days**.
5. Choose the **Role**: *Member: can read the organization*, *Admin: can manage people, teams and
   access*, or *Owner: can manage; an organization always keeps at least one*.
6. Press **Send invitation**.

Lake does not send an email. Tell the person: they join from their **Organization →
Organizations** page once they sign in with that email address, or with the account that has that
handle.

- REST: `POST /v1/create_invitation` with
  `{"organization_id": "<organization id>", "invitee_type": "email", "invitee": "ada@example.com", "role": "member", "expires_at": "2026-10-17T12:00:00Z"}`.
  `expires_at` is a UTC instant; the page turns its day count into one.
- MCP: `create_invitation` with the same input.

To invite into a team instead of the whole organization, add `"team_id": "<team id>"`.

The **Invitations** tab lists what you sent with its status (pending or accepted) and its expiry.

- REST: `GET /v1/list_invitations?organization_id=<organization id>&limit=100`.
- MCP: `list_invitations`.

## Join an Organization You Were Invited To

1. Sign in with the invited email address (or as the account with the invited handle).
2. Open **Organization → Organizations**. A banner says how many invitations wait for you; press
   **Review**, or press **Join**.
3. In **Invitations for You**, press **Join** beside the organization.

The organization's page opens. Your membership records the invitation as its evidence and the
person who invited you as the grantor.

- REST: list what waits for you with `GET /v1/list_invitations` (no input), then
  `POST /v1/join_organization` with `{"invitation_id": "<invitation id>"}`.
- MCP: `list_invitations`, then `join_organization`.

## Create a Team

1. Open the organization's **Teams** tab and press **New team**.
2. Enter a **Team name** (lowercase letters, numbers and inner hyphens, such as `reviewers`) and an
   optional **Display name**.
3. Press **Create team**.

- REST: `POST /v1/create_team` with
  `{"organization_id": "<organization id>", "name": "reviewers", "display_name": "Reviewers"}`.
- MCP: `create_team`.

A team's page (**Team Settings**) lists its members and its effective access. People join a team
through an invitation with its `team_id`; roles on an organization flow down to its teams.

- REST: `GET /v1/list_teams?organization_id=<organization id>`.
- MCP: `list_teams`.

## Change a Member's Role

1. Open the organization's **Members** tab and select the member.
2. On the member page, under **Change role**, choose `member`, `admin` or `owner`.
3. Press **Change role**.

- REST: `POST /v1/set_member_role` with `{"id": "<membership id>", "role": "admin"}`.
- MCP: `set_member_role`.

The membership's derived grant is replaced by the new role's. Lake refuses to demote the last owner
of an organization or team.

## Remove a Member

1. Open the member's page from the **Members** tab.
2. Press **Remove member**, read *Removing this member revokes their membership and derived grants
   immediately*, and confirm with **Remove member**.

- REST: `POST /v1/remove_member` with `{"id": "<membership id>"}`.
- MCP: `remove_member`.

The membership, its member object and the grant derived from it are removed at once. Lake refuses
to remove the last owner, including when that owner is you.

## Permissions

| Action | Needs |
|---|---|
| See an organization, its teams and members | Any membership (a `read` grant) |
| Invite, create teams, change roles, remove members, add SSO | `admin` on the organization (role admin or owner) |
| Create an organization | Being signed in as a person (an operator credential is refused) |
| Turn your account into an organization | Being the person whose account it is |
| Join an organization | Being the person the invitation is addressed to |

Every change is recorded in the central audit log.

## Limits

- An invitation lasts 1 to 90 days through the page (1, 7 or 30 offered). After it expires it can no
  longer be used; send a new one.
- An invitation can be used once.
- An organization and each team always keep at least one owner.
- Lake sends no invitation email; share the invitation yourself.
- The page counts members per organization card only while you see 24 organizations or fewer.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| `invalid_input` at `/name` when creating an organization | The handle is not lowercase letters, numbers and inner hyphens. |
| `conflict` when creating an organization | The handle is taken. Choose another, or leave it empty. |
| `unauthorized` when converting | It is not your own account, or you are signed in with the operator credential. |
| `conflict` when converting | That account already belongs to an organization. |
| `unauthorized` on **Join** | The invitation is for another email or handle, or it expired. |
| `conflict` on **Join** | The invitation was already used. |
| `unauthorized` on invite, role change or removal | You are a member, not an admin or owner. A member sees the refusal inside the Invitations tab; the rest of the page still opens. |
| A role change or removal is refused | It would leave the organization or team without an owner. Make someone else owner first. |
| **No invitations waiting** | You are signed in with another email, or the invitation went to a handle. Ask for an invitation to the address you sign in with. |

## Related Pages

- [Grants and Effective Access](/docs/access)
- [Single Sign-On and SCIM](/docs/sso)
- [Workspaces](/docs/workspaces)
- [Usage and Metering](/docs/usage)
- [Activity and Audit](/docs/activity)
- [Security Model](/docs/security)
