# Organizations, teams, membership, and invitations

**What.** Organizations contain teams. A membership places a user or workload in an
organization or one of its teams with the role `owner`, `admin`, or `member`. Every membership
stores the digest of its `org_membership/1` TAAP attestation and the TAAP identity of the
administrator who granted it. Email and handle invitations expire and become memberships only
through `accept_invitation`.

| Capability | Kind | Required input | Result |
|---|---|---|---|
| `list_organizations` / `get_organization` | read | `limit` / `id` | organizations; `get_organization` also names the organization's own space as `customer_id` (null when it has none, lake#561) |
| `create_organization` | action | `taap_id?`, `display_name?`, `customer_id?` | organization |
| `list_teams` / `get_team` | read | `organization_id` / `id` | teams |
| `create_team` | action | `organization_id`, `name`, `display_name?` | team |
| `list_memberships` / `get_membership` | read | scope (omit for your own) and `id` | memberships |
| `create_membership` | action | organization, optional team, principal, role, attestation digest | membership |
| `set_member_role` | action | membership `id`, role | membership |
| `remove_member` | action | membership `id` | `{removed: true}` |
| `list_invitations` | read | `organization_id?`, `team_id?`, `limit?` (omit the scope for yours) | invitations |
| `create_invitation` | action | organization, optional team, email or handle, role, expiry | invitation |
| `accept_invitation` | action | invitation, principal, attestation digest | membership |
| `create_organization_space` | action | `display_name`, `name?` | `{organization, customer, membership}` |
| `convert_personal_space` | action | `customer_id`, `display_name` | `{organization, customer, membership, personal_customer}` |
| `join_organization` | action | `invitation_id` | membership |

All appear under `/v1`, in `/v1/openapi.json`, and as MCP tools. The server-rendered views are
`/organizations`, `/teams?organization_id=…`, `/members?organization_id=…&team_id=…`, and
`/invitations?organization_id=…&team_id=…`. They call these capabilities through the registry;
they do not read the store directly. The browser forms are described in
[`../ui/settings.md`](../ui/settings.md).

## Self-service: an organization a person runs

These capabilities serve a signed-in person (a session or token with a user). An operator
credential has no person behind it, so they refuse it; operators keep `create_organization` and
`create_customer`.

- `create_organization_space` creates, in one transaction, the organization, its own space (an
  `organization` customer; `name` is its handle and is generated when omitted), the caller's
  `owner` membership, and the derived grants. The membership's evidence is
  `create_organization_space/<request id>`, granted by the caller, like the
  `sign_up_password/<document hash>` evidence of sign-up provisioning: a provenance reference,
  not a new attestation format. An invalid handle answers `400`, a taken one `409`.
- `convert_personal_space` turns the caller's own personal space (an `individual` customer whose
  `user_id` is the caller) into an organization's space. The customer keeps its id and handle, so
  its repositories, stores, workspaces, apps and links stay where they are; it becomes
  `customer_type: organization` with a new organization, and the caller becomes that
  organization's `owner`. In the same transaction lake provisions the caller a fresh personal space
  with a generated handle and an `admin` grant, as sign-up does. Someone else's space answers
  `401`; a space that is already an organization's answers `409`. The browser form also asks the
  person to type the handle before it calls the capability. The caller's existing direct `admin`
  grant on the converted space is kept.
- `list_invitations` without `organization_id` returns the pending, unexpired invitations
  addressed to the caller: `email` invitations matching the caller's email, case-insensitively,
  and `handle` invitations matching the handle of the caller's personal space. Each item also
  carries `organization_name`, because the invitee cannot read the organization until they join.
- `join_organization` accepts one of those invitations as the caller. The membership's
  `attestation_digest` is `invitation/<invitation id>` and its `granted_by_taap_id` is the
  inviter. An invitation addressed to someone else, or expired, answers `401`; one already used
  answers `409`. `accept_invitation` remains the administrator's path for accepting on behalf of
  a principal with separately produced evidence.
- `list_memberships` without `organization_id` returns the caller's own memberships across
  organizations and teams. With neither a scope nor a person (an operator), both list
  capabilities answer `400 organization_id is required …`.

`get_customer` accepts `customer_id` as well as `id` (see [customers.md](customers.md)).

## Delegated administration

An `owner` or `admin` membership creates an `admin` grant on its organization or team object. A
`member` membership creates a `read` grant. An organization administrator can therefore create
teams, members, invitations, and grants below that organization, but cannot act on a sibling
organization or on a customer that merely parents it. A customer administrator can create an
organization with `customer_id`; the new organization object becomes a child of that customer.

**An organization's own space.** When the organization has its own space (the `organization`
customer whose `organization_id` is that organization), an organization-scope membership also
derives the same grant, with the same evidence, on that space's customer object. Owners and
admins therefore administer the space and its repositories, and members read them; a team
membership does not reach the space. Every store path keeps the two grants together:
`create_membership`, `accept_invitation`, `join_organization`, `create_organization_space`,
`convert_personal_space`, `set_member_role`, and `remove_member`. When the principal already
holds that role on the space by another grant (for example, the sign-up grant of a converted
personal space), no second grant is written and that grant is left alone on removal.
Memberships created before this rule have no space grant; there is no backfill.

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

`set_member_role` replaces the membership's derived organization or team grant with the role's
new policy grant. It refuses to demote the final `owner` in a scope. `remove_member` removes the
membership, its member policy object, and the matching derived grant. It refuses to remove a final
owner, including when that owner is the acting principal. Both actions require `admin` on the
membership's organization or team and are recorded by the registry audit path.

## Verify and change

Run the unit and Postgres tests documented in [`../build/ci.md`](../build/ci.md). The acceptance
coverage is in `tests/test_policy.cpp`; the production-store capability round trip is in
`tests/test_postgres.cpp`. Self-service is covered by
`Organizations.APersonCreatesAnOrganizationInvitesAndAColleagueJoinsItsSpace` and
`Organizations.APersonalSpaceBecomesAnOrganizationAndItsOwnerGetsAFreshOne` (memory store),
`PostgresCapabilities.OrganizationSpaceMembershipsCarryTheirGrantToTheSpace`, and the browser
flows `HttpResponseIsolation.PersonCreatesAnOrganizationInvitesAndAColleagueJoins` and
`HttpResponseIsolation.PersonalSpaceBecomesAnOrganizationAfterConfirmation` (memory and
PostgreSQL). Add organization behavior in the store interface first, then expose a
capability with object/action metadata and add its page renderer. Schema changes require the
numbered migration and operator copy described in [`../control-plane/schema.md`](../control-plane/schema.md).
