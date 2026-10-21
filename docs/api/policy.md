# Policy engine and effective access

**What.** One object graph and one authorization function govern repository, drive, and platform
access. `lake::control::authorize(principal, object, action)` lives in `src/control/policy.cpp`.
`Registry::call` invokes it before every protected capability handler, using the capability's
required object resolver and action. REST, OpenAPI, MCP, and server-rendered pages therefore use
the same decision.

The graph stores customer, organization, team, member, repository, path, environment, secret
mount, and secret objects. Each object has one optional parent and an `inherit` flag. A request
walks from the target toward its ancestors. The target is always considered; encountering
`inherit = false` includes that object's direct grants and stops before its parent. Grants above
that cut do not apply.

`AuthorizationCache` belongs to one `ApiContext`. It caches ancestor paths and the grants loaded
for a principal, so repeated checks in a request do not repeat graph reads. `authorize_batch`
loads paths and grants for a candidate set in batches. `list_objects`, organizations, customers,
repositories, and search filter every candidate with it, including children that cut inheritance.
Scoped lists authorize their scope with the same batched call before returning rows.

Row-resolved browser and API-token credentials carry the user's TAAP ID into this same function.
Identity-management capabilities resolve the synthetic object `principal/<taap_id>`; only that
principal receives its `admin` action, so one user cannot manage another user's sessions, devices,
passkeys, or tokens. `authorize_batch` preserves that self-principal decision, so session and
device lists can use the batched path before returning rows. API-token capability names are
checked as scopes before policy evaluation;
the operator bearer remains the explicit break-glass principal. For `create_token`, passing the
capability-name check does not grant arbitrary delegation: requested scopes must be a subset of
the issuing credential, explicit expiry must fit its remaining lifetime, and a static child
requires existing static authority. These checks live in the capability handler and are audited
by the registry. The store retains the issuing session and rejects descendants after any
ancestor or linked device is revoked or expires. The complete derivation and migration contract is
[`identity.md`](identity.md#token-derivation-and-revocation).

## Principal ids (lake#870)

**What.** The principal `admin` is the operator's: `authorize` allows it everything, and the
registry skips the policy check for it. Only the admin token acts as it. Every identity a person,
organization or grant carries is a TAAP id: `taap:` followed by printable ASCII, at most 512
characters (`valid_principal_id`, `src/control/policy.cpp`).

- **Creation refuses anything else** with `invalid_input`: `create_user`, `create_organization`,
  `upsert_scim_user` and the just-in-time user of `sign_in_sso`. Before this, those paths stored a
  caller-chosen `taap_id` as given, so a person who registered their own SSO provider could create a
  user named `admin` and receive an operator session (lake#870).
- **Authentication refuses it too.** Each store's `context_for` passes the principal it resolved
  through `accepted_principal`: a credential whose identity is `admin`, or a person's credential
  whose identity is not a TAAP id, is answered as an unknown credential (`not_found`). That covers a
  row written outside lake, such as an old row or a direct database write. Workload credentials
  (`app_`, `build_`, `methodology_`) name their workload and only the `admin` check applies to them.
  An agent stream trigger acts as its person only when that person's identity is a TAAP id.

**Why.** Policy decisions are keyed by the principal string, so a stored identity equal to `admin`,
or to an identity lake sets in code (`agent-service:`, `share-link:`, `sso-provider/`), would inherit
what that identity may do. The remaining work on unscoped principals is lake#872.

**Verify.** `PrincipalIds.*` (`tests/test_sso.cpp`) and
`PostgresIdentity.OperatorPrincipalIsNeverAPersonsIdentity` (`tests/test_postgres_identity.cpp`),
which plants a user named `admin` in the database and shows its session is refused.

## Objects and grants

| Capability | Authorization | Result |
|---|---|---|
| `list_objects` | `read` on `parent_id`; operator admin may list roots | child objects |
| `get_object` | `read` on `id` | one object |
| `create_object` | `admin` on `parent_id` | child object |
| `list_grants` | `admin` on `object_id` | direct grants |
| `create_grant` | `admin` on `object_id` | grant, with the attestation lake signed for it |
| `revoke_grant` | `admin` on the grant's object | the revoked grant ([below](#revoking-a-grant)) |
| `effective_access` | `share` on `object_id` | direct and inherited access |
| `entity_effective_access` | `share` on the resolved repository, organization, or team | its policy object and direct and inherited access |

The pages are `/grants?object_id=…` and `/access?object_id=…`. Repository, organization, and
team pages link to `/repository/access?id=…`, `/organization/access?id=…`, and
`/team/access?id=…` through `entity_effective_access`. Effective access returns the
principal, role, allowed actions, source object, and whether the grant is inherited. This is the
sharing view required by the product design.

## Revoking a grant

`revoke_grant {grant_id, dry_run?}` (lake#645) needs `admin` on the grant's object, as
`create_grant` does, and removes that one grant; the answer is the grant as it was, with
`dry_run`. Two grants are kept, each `409 conflict` with the reason: one a membership carries
(the same principal and attestation digest as a membership), which goes with the membership
(`remove_member`), and the last `admin` grant on an account (`customer` object), so an account always
has an administrator. The event is central (`grant.revoke`, naming the principal, object and
role).

## The grant's attestation (lake#602)

`create_grant` takes `principal_taap_id`, `object_id` and `role`. Lake writes the grant's evidence
itself, as `share_drive_folder` does ([sharing.md](sharing.md#the-grants-attestation-lake585)): the
statement

`{type: "lake.grant/1", object_id, principal_taap_id, role, granted_by, issuer}`

signed with lake's TAAP identity (purpose `grant`), and keeps `sha256:<hex>` of the JSON of
`{statement, signer, signature}` as the grant's `attestation_digest`. The answer is the grant plus
`attestation: {statement, signer, signature, digest}`, and the registry audit event carries the same
`attestation`, so anyone holding the audit can recompute the digest and check the signature against
lake's identity document. The statement has no time and Ed25519 signatures are deterministic, so
`attestation_digest` stays an optional input for programs that repeat a call: it is accepted only
when it equals lake's digest for the same object, principal, role and granter, and anything else is
`invalid_input` at `/attestation_digest` with nothing written. Before lake#602 the caller supplied
the digest and lake stored whatever it was sent; the Grants and Members pages showed that string as
"attestation evidence" although it proved nothing.

`create_membership` and the administrator's `accept_invitation` follow the same rule with the
statement `lake.org_membership/1` (purpose `org-membership`); see
[organizations.md](organizations.md#attestation-evidence). The shared code is
`src/api/lake_attestation.cpp` (`sign_lake_attestation`, `check_supplied_digest`).
`Organizations.GrantsAndMembershipsKeepTheDigestOfAStatementLakeSigned` (`tests/test_api.cpp`)
covers all three: a made-up digest is refused and writes nothing, lake's digest recomputes from the
answer, and a digest for another role is refused. Verify on a running lake with
`POST /v1/create_grant` without `attestation_digest`: the answer's `attestation.digest` equals the
grant's `attestation_digest`.

Grants lake writes for itself with an evidence reference rather than a caller's digest
(`sign_up_password/…`, `invitation/<id>` for `join_organization`, `create_organization_space/…`,
`scim:…`) are unchanged; they never took the digest from the caller.

## Roles by object type

`admin` and `read` apply to every object type. Other roles are constrained to objects where the
action has meaning:

| Object type | Additional roles |
|---|---|
| customer | `write`, `share` |
| organization, team | `write`, `share` |
| member | none |
| repository, path | `write`, `review`, `share` |
| environment | `write`, `deploy`, `share` |
| secret mount, secret | `write`, `share` |

Role evaluation is fixed: `read` permits read; `write` permits read and write; `deploy`, `review`,
and `share` permit read plus their named action; `admin` permits all six actions. Creating a grant
validates the role against the target's object type.

## Object topology

The store validates parent types: customer is a root; customer → organization; organization →
team; organization or team → member; customer, organization, or team → repository; repository or
path → path; customer, organization, team, or repository → environment; environment → secret
mount; secret mount → secret. Existing customers, organizations, and repositories are backfilled
by migration `0004_organizations_policy`, and their create operations add the matching object in
the same transaction.

Creating a secret mount adds a stable environment below its owner and the mount below that: an
organization's mount gets the organization child `secrets/<customer-id>/<organization-id>`, and a
mount the account owns (no `organization_id`, lake#561) gets the customer child
`secrets/<customer-id>`. Per-name `secret` objects are no longer created (lake#328). Mount capabilities authorize against the mount object; mount lists batch-filter
their candidates. See [`secrets.md`](secrets.md).

## Verify and change

`Policy.InheritsDownTheGraphAndStopsAtACut`,
`Policy.DelegatedAdministratorCannotLeaveItsObjectScope`, and
`Policy.EffectiveAccessMatchesAuthorization`, the identity scope test, and the WebAuthn test are
the executable contract. The registry test also
enumerates every capability and requires object/action metadata. Run the Postgres fixture from
[`../build/ci.md`](../build/ci.md) to exercise the recursive query used in production.

New capabilities must name their object, action, and object resolver. New list or search paths
must call `authorize_batch` on candidates before returning them. Do not add authorization in an
HTTP adapter or page renderer.
