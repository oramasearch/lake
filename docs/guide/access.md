# Grants and Effective Access

Lake decides every call the same way, whether it comes from the web app, REST or MCP: it asks
whether *this person* may take *this action* on *this object*. This page explains the objects, the
roles, how access is inherited, and the two pages that show it: **Direct Grants** and **Effective
Access**.

## Concepts

| Term | What it is |
|---|---|
| **Object** | Something access is granted on: an account (the owner of workspaces), an organization, a team, a member, a repository, a path in a repository, an environment, a secret mount or a secret. Each object has one parent. |
| **Action** | What a call does to an object: `read`, `write`, `admin`, `deploy`, `review` or `share`. |
| **Role** | What a grant gives. `read` permits read; `write` permits read and write; `deploy`, `review` and `share` permit read plus their own action; `admin` permits all six. |
| **Grant** | One role on one object for one principal (a person's TAAP identity), backed by the digest of a signed attestation and the identity of whoever granted it. |
| **Direct grant** | A grant attached to the object itself. |
| **Inherited access** | Access that comes from a grant on an object above it, such as the organization above a team, or the account above a repository. |
| **Inheritance cut** | An object can stop inheritance (`inherit` false). Grants on that object still count; grants above it do not. |
| **Effective access** | Everyone who can reach an object, their role, what they may do, which object the grant is on, and whether it is inherited. |

## How Objects Are Arranged

The tree of objects decides what flows where:

- an account is a root;
- account → organization → team;
- organization or team → member;
- account, organization or team → repository; repository or path → path;
- account, organization, team or repository → environment;
- environment → secret mount → secret.

A request walks from the object up through its parents and collects every grant the person holds,
stopping after an object that cuts inheritance. A role on an organization therefore reaches its
teams, and a role on an account reaches its repositories, unless something in between cuts it.

Not every role makes sense everywhere. `admin` and `read` apply to every object; the others are
limited:

| Object | Extra roles |
|---|---|
| account, organization, team | `write`, `share` |
| repository, path | `write`, `review`, `share` |
| environment | `write`, `deploy`, `share` |
| secret mount, secret | `write`, `share` |
| member | none |

## Where Grants Come From

Most grants are made for you:

- **Signing up** gives you `admin` on your own account.
- **An organization membership** gives `admin` (owner or admin) or `read` (member) on the
  organization, and the same on the organization's own account
  ([Organizations, Members and Teams](/docs/organizations#roles)). A team membership gives the
  grant on the team.
- **Sharing a folder** gives a person a role on that folder ([Sharing and Shared with Me](/docs/sharing)).

You can also make a grant directly through the API.

## See Who Can Reach Something

**On a repository:** open the repository; its **Effective access** card shows who reaches it. Press
**View access details** for the full page.

**On an organization:** open **Organization → Organizations**, select the organization and open the
**Access** tab, or press **Open effective access**.

**On a team:** open the team's **Team Settings** page; the effective access is listed there.

The **Effective Access** page shows figures (principals, direct and inherited entries), a search and
a role filter, and one row per principal with the role, the actions it allows and whether it is
direct or inherited.

- REST: `GET /v1/entity_effective_access?object_type=repository&id=<repository id>` (`object_type`
  is `repository`, `organization` or `team`).
- MCP: `entity_effective_access` with the same input.

For any policy object by its object id:

- REST: `GET /v1/effective_access?object_id=<object id>`.
- MCP: `effective_access`.

## See the Direct Grants on an Object

The **Direct Grants** page (`/grants?object_id=<object id>`) lists the grants attached to one
object: principal, role, attestation digest and who granted it. Press **View effective access** to
see everything that reaches it, inherited grants included.

- REST: `GET /v1/list_grants?object_id=<object id>&limit=100`.
- MCP: `list_grants`.

To find an object's id, read it with `GET /v1/get_object?id=<object id>`, or list the children of
an object with `GET /v1/list_objects?parent_id=<object id>&limit=100`.

## Make a Grant

There is no form for this in the web app: grants are made by memberships, sharing, or this call. You
need `admin` on the object.

- REST: `POST /v1/create_grant` with
  `{"principal_taap_id": "<TAAP identity>", "object_id": "<object id>", "role": "read", "attestation_digest": "<attestation digest>"}`.
- MCP: `create_grant` with the same input.

`role` must be one the object's type allows (see the table above); anything else is refused with
`invalid_input`. `attestation_digest` names the signed attestation that justifies the grant, kept
with it as evidence.

## Check What You Can Do

The web app shows a button only when the action would work, using `my_permissions`. You can ask the
same question:

- REST: `POST /v1/my_permissions` with
  `{"object_type": "customer", "object_id": "<customer id>", "actions": ["read", "write", "admin"]}`.
- MCP: `my_permissions`.

The answer is `allowed`, one true or false per action. `object_type` is a policy object type
(`customer` for an account, `repository`, `organization`, `team`, …) or `drive folder`, which names
a repository by `object_id` and a folder by `path` at `ref`. The actions are `read`, `write`,
`admin`, `deploy`, `review`, `share` and `promote` (`promote` is `admin` on the object, used for
promoting experiment trials). It never replaces the check each capability makes.

## Permissions

| Call | Needs |
|---|---|
| `effective_access`, `entity_effective_access` | `share` on the object |
| `list_grants`, `create_grant` | `admin` on the object |
| `get_object`, `list_objects` | `read` on the object or its parent |
| `my_permissions` | being signed in |

Lists everywhere in lake (repositories, organizations, search results) only return what you may
read: each candidate is checked before it is returned.

## Tokens Follow the Same Rules

An API token or MCP key acts as the person who made it. Its scopes limit which capabilities it may
call; the person's grants still decide which objects. A token never does more than the person
([Tokens and Keys](/docs/tokens)).

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| `unauthorized` (403) | Your credential is valid but no grant reaches the object with that action. Check **Effective Access** for the object, or ask an admin. |
| `not_found` | The object does not exist, or you may not see it. Lake does not tell the two apart for objects you cannot read. |
| `invalid_input` on `create_grant` | The role is not allowed on that object type, or a field is missing. |
| Access you expected is not inherited | An object between the grant and the target cuts inheritance. Look for the cut on the **Effective Access** page's source column. |
| A removed member still appears | Their access came from another grant too, such as a direct grant or a second membership. |

## Related Pages

- [Organizations, Members and Teams](/docs/organizations)
- [Sharing and Shared with Me](/docs/sharing)
- [Tokens and Keys](/docs/tokens)
- [Security Model](/docs/security)
- [Activity and Audit](/docs/activity)
