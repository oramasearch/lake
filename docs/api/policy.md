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
the operator bearer remains the explicit break-glass principal.

## Objects and grants

| Capability | Authorization | Result |
|---|---|---|
| `list_objects` | `read` on `parent_id`; operator admin may list roots | child objects |
| `get_object` | `read` on `id` | one object |
| `create_object` | `admin` on `parent_id` | child object |
| `list_grants` | `admin` on `object_id` | direct grants |
| `create_grant` | `admin` on `object_id` | attestation-backed grant |
| `effective_access` | `share` on `object_id` | direct and inherited access |
| `entity_effective_access` | `share` on the resolved repository, organization, or team | its policy object and direct and inherited access |

The pages are `/grants?object_id=…` and `/access?object_id=…`. Repository, organization, and
team pages link to `/repository/access?id=…`, `/organization/access?id=…`, and
`/team/access?id=…` through `entity_effective_access`. Effective access returns the
principal, role, allowed actions, source object, and whether the grant is inherited. This is the
sharing view required by the product design.

## Roles by object type

`admin` and `read` apply to every object type. Other roles are constrained to objects where the
action has meaning:

| Object type | Additional roles |
|---|---|
| customer | `share` |
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

Creating a secret mount adds a stable organization child environment named
`secrets/<customer-id>/<organization-id>` and the mount below it. A first put adds the named secret
below the mount. Mount capabilities authorize against the mount object; mount lists batch-filter
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
