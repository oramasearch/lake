# Organizations, teams, membership, and invitations

**What.** Organizations contain teams. A membership places a user or workload in an
organization or one of its teams with the role `owner`, `admin`, or `member`. Every membership
stores the digest of its `org_membership/1` TAAP attestation and the TAAP identity of the
administrator who granted it. Email and handle invitations expire and become memberships only
through `accept_invitation`.

| Capability | Kind | Required input | Result |
|---|---|---|---|
| `list_organizations` / `get_organization` | read | `limit` / `id` | organizations |
| `create_organization` | action | `taap_id?`, `display_name?`, `customer_id?` | organization |
| `list_teams` / `get_team` | read | `organization_id` / `id` | teams |
| `create_team` | action | `organization_id`, `name`, `display_name?` | team |
| `list_memberships` | read | `organization_id`, `team_id?`, `limit?` | memberships |
| `create_membership` | action | organization, optional team, principal, role, attestation digest | membership |
| `list_invitations` | read | `organization_id`, `team_id?`, `limit?` | invitations |
| `create_invitation` | action | organization, optional team, email or handle, role, expiry | invitation |
| `accept_invitation` | action | invitation, principal, attestation digest | membership |

All appear under `/v1`, in `/v1/openapi.json`, and as MCP tools. The server-rendered views are
`/organizations`, `/teams?organization_id=…`, `/members?organization_id=…&team_id=…`, and
`/invitations?organization_id=…&team_id=…`. They call these capabilities through the registry;
they do not read the store directly.

## Delegated administration

An `owner` or `admin` membership creates an `admin` grant on its organization or team object. A
`member` membership creates a `read` grant. An organization administrator can therefore create
teams, members, invitations, and grants below that organization, but cannot act on a sibling
organization or its customer parent. A customer administrator can create an organization with
`customer_id`; the new organization object becomes a child of that customer.

The operator admin token remains a break-glass credential and is evaluated by the same
`authorize` function. User credentials provide their TAAP principal through the shared request
context when the identity work lands.

## Attestation evidence

`attestation_digest` identifies the canonical signed `org_membership/1` attestation and
`granted_by_taap_id` records its authority. Those two values are retained on both the membership
and the policy grant, so a verifier with the attestation and identity document can verify the
statement offline. Lake does not invent another signature envelope. The identity core owns
attestation storage and TAAP signature verification; this API retains the stable evidence link
needed by that verifier.

Invitation acceptance is transactional: it locks the pending invitation, rejects expired or
already-used invitations, creates the membership, member object, and corresponding grant, then
marks the invitation accepted.

## Verify and change

Run the unit and Postgres tests documented in [`../build/ci.md`](../build/ci.md). The acceptance
coverage is in `tests/test_policy.cpp`; the production-store capability round trip is in
`tests/test_postgres.cpp`. Add organization behavior in the store interface first, then expose a
capability with object/action metadata and add its page renderer. Schema changes require the
numbered migration and operator copy described in [`../control-plane/schema.md`](../control-plane/schema.md).
