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

## Principal ids (lake#870, lake#872)

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

- **Grants and memberships someone writes name TAAP identities** (lake#872): `create_grant`,
  `create_membership` and imported grants refuse (or, on import, skip) `admin`, bare names and
  lake's own synthetic principals. `share-link:<id>` is one: a share link's folder is its first
  grant, so a grant written for another tenant's link id could have repointed it. Lake still creates
  `share-link:` grants itself when a link is made.

- **Nobody but Lake chooses a new SSO or SCIM account's identity** (lake#872): the account a
  just-in-time sign-in or a SCIM create makes gets a managed key Lake mints, and any `taap_id` or
  `taapId` the caller sends is ignored ([SSO](sso.md)). Before, an unauthenticated SAML or OIDC
  caller, or an organization's SCIM client, named it without holding its key.
- **One principal id, one principal** (lake#872): `create_user`, `create_organization`, the SCIM
  create and the just-in-time user answer `conflict` for an identity any user (in any pool) or
  organization already has, so no new principal inherits another's grants or name.
- **The database refuses anything else** (lake#872, migration `0109_principal_ids`):
  `users_taap_id_format` and `organizations_taap_id_format` check the same rule, `NOT VALID` so
  rows from before lake#870 are not scanned; a write to such a row is refused until it is
  corrected. The operator must vendor the schema and select `principal_ids_guard`.

**Why.** Policy decisions are keyed by the principal string, so a stored identity equal to `admin`,
or to an identity lake sets in code (`agent-service:`, `share-link:`, `sso-provider/`), would inherit
what that identity may do, and so would a new account given another principal's identity.

**Grants name identities, not accounts, on purpose.** A grant names a TAAP id, not a user id and
pool. Since lake#912 a session of an application pool carries no Lake grants, and every creation
path above now validates the id, takes it from a key Lake minted or a proof of the key, and
refuses one another principal holds. A grant on a TAAP id therefore reaches only the Lake-pool
account holding that key; naming user ids would also break grants made before an account exists
(invitations, imported grants) and identities that move between accounts (*Use my own
identity*).

**Verify.** `PrincipalIds.*` (`tests/test_sso.cpp`) and
`PostgresIdentity.OperatorPrincipalIsNeverAPersonsIdentity` (`tests/test_postgres_identity.cpp`),
which shows the database refuses a user named `admin`, then plants one as an old row would be and
shows its session is refused. `PrincipalIds.NoCreationPathTakesAnIdentityAnotherPrincipalHolds`
covers collisions, `PrincipalIds.NoCreationPathAcceptsTheOperatorIdentity` a SCIM client's
ignored `taap_id`, and `SamlSignIn.TheAcsSignsInOnceAndRefusesReplayTamperingAndUnsolicitedResponses`
the managed key of a just-in-time SAML account.

## Objects and grants

| Capability | Authorization | Result |
|---|---|---|
| `list_objects` | `read` on `parent_id`; operator admin may list roots | child objects |
| `get_object` | `read` on `id` | one object |
| `create_object` | `admin` on `parent_id`; a `path` named for the repository it sits in ([below](#an-object-counts-only-inside-its-own-account-lake913)), for a node of its tree at `ref` (default `main`), under the object of the nearest folder above that node that has one ([below](#a-path-object-sits-where-its-node-lies-lake1030)) | child object |
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

## An object counts only inside its own account (lake#913)

**What.** An object's `(object_type, external_id)` is unique across every account, and lake finds
the object governing a resource by that pair. Several external ids are derived from identifiers
other accounts can learn: a repository folder is `<repository_id>:<node_id>`, a branch rule
`<repository_id>:<ref>`, an environment `md5(<repository_id>\n<name>)`, an account's secret mounts
sit under `secrets/<customer_id>`. Before lake#913, `create_object` took any type and any external
id under any parent the caller administered, so an account could create those objects first, under
its own repository or customer, and govern another account's folders, future environments or
future secret mounts with its own grants (finding LSR-002 of the 2026-10 security review).

**The rule, in two places.**

- `create_object` adds only `path` objects, and a path's external id must begin with the id of
  the repository its parent sits in (`<repository_id>:`). Any other type is `invalid_input`: lake
  creates environments, secret mounts and the account objects itself, from the entity they belong
  to. The operator credential may still create the other types, for repair; its paths follow the
  same rule.
- Every place that finds an object by a derived id checks that the object sits under the entity it
  names, and never adopts one that does not:

  | Where | The object must sit | Otherwise |
  |---|---|---|
  | the drive capabilities (`nearest_drive_location`, folder lists, share links) | under the repository's object | ignored: the repository's own grants decide |
  | sharing a folder (`ensure_folder_location`) | under the repository's object | `conflict` |
  | `set_branch_protection` | directly under the repository's object | `conflict` |
  | environments (`environment_policy_object`: `ensure_environment` and the environment capabilities) | directly under the repository's object | `conflict` |
  | `create_secret_mount_route` (both stores) | the owner's `secrets/…` environment directly under the owner's object | `conflict`, nothing written |
  | the secret mount capabilities and `list_secret_mounts` | under the owning customer's object | `conflict`; the list leaves it out (the operator still sees it) |

A `conflict` names the object and asks for the operator: the identifier is unique, so a squatted
object can be neither adopted nor replaced until it is removed.

**Finding objects made before the fix.** A read-only query on the control database lists the
objects whose derived id names a repository they do not sit under:

```sql
WITH RECURSIVE chain AS (
  SELECT o.id AS start, o.id, o.parent_id FROM objects o WHERE o.object_type = 'path'
  UNION ALL
  SELECT c.start, p.id, p.parent_id FROM chain c JOIN objects p ON p.id = c.parent_id
)
SELECT o.id, o.object_type, o.external_id FROM objects o
WHERE o.object_type = 'path'
  AND NOT EXISTS (SELECT 1 FROM chain c JOIN objects r ON r.id = c.id
                  WHERE c.start = o.id AND r.object_type = 'repository'
                    AND o.external_id LIKE r.external_id || ':%');
```

Environments are checked the same way against `md5(repository_id || E'\n' || name)`, and
`secrets/<customer_id>…` environments against their parent's customer. Removing a squatted object
deletes data and needs the operator's explicit decision.

**Verify.** `PolicyObjectOwnership.MemoryStore` and `PolicyObjectOwnership.PostgresStore` run the
same case against each store: one account's `create_object` is refused for another account's
folder, environment and `secrets/<customer_id>` ids, and objects planted before the fix (directly
in the store) neither let it upload into the other account's folder nor claim its branch rule,
environment or secret mounts, while the owner keeps its access.

## A path object sits where its node lies (lake#1030)

**What.** After lake#913 a path object's external id had to name the repository its parent sits in,
but not where in the repository. Admin on one folder's object let its holder call `create_object`
under that folder with the node id of a sibling folder, or of any other node of the repository;
the object counted for that node, and the folder administrator's grants governed it.

**The rule.** For every caller but the operator (who keeps it for repair), `create_object` reads
the repository's tree at `ref` (default `main`) and refuses a `path` object (`invalid_input`) when:

- no file or folder of that tree has the node id the external id names (`/external_id`); a branch
  rule's `<repository_id>:<ref>` names no node, so it is refused too, and only
  `set_branch_protection` makes those;
- the parent is not the object of the nearest folder above the node that has one in this
  repository, or the repository's own object when none does (`/parent_id`). That is where the
  drive capabilities link folder objects (`link_folder_chain`), so a path object is made only by
  an administrator of the folder it lies in, never beside it or below another folder's object.

A tombstoned repository is `conflict`; no repository store is `store_unavailable`.

**Verify.** `Drive.CreateObjectPlacesAPathObjectByTheTree`: a folder administrator cannot claim a
file beside the folder, an unknown node or a branch rule; a repository administrator cannot place
an object for a file inside a folder that has its own object under the repository; each places the
objects of its own folder.

## Restricted folders hold for every repository read (lake#975)

**What.** A `path` object with `inherit = false` cuts its folder or file off from the
repository's grants. Drive always decided a path by the nearest object on it that counts
(`nearest_drive_location`). Before lake#975 the other reads of repository content authorized only
the repository, so a repository reader without a grant on the cut still read what it protects
(finding LSR-108 of the 2026-10 security review). Now every one decides each path the way Drive
does, through `RepositoryPathAccess` (`src/api/repository_path_access.cpp`):

| Read | What a cut the caller cannot read does |
|---|---|
| `repository_file` | `unauthorized` at `/path` |
| `repository_tree` | its entries are left out |
| `change_diff` | an entry is left out unless every path it names is readable at a revision that holds it (the change, or its first parent for a move or a delete) |
| `list_conflicts` | a conflict is left out unless its path is readable at the sides that hold it |
| `search` | a hit is left out, decided at the change it was found in |
| Drive search (`search_drives`) | names under the cut are not found |
| `pull`, `read_chunk`, `export_repository` | `unauthorized`: they carry every path and cannot leave a folder out, so they need `read` on every cut of the repository |

**How.** `RepositoryPathAccess::load` reads the repository's path objects once, level by level
below the repository's own object (only objects there count, lake#913). A repository with no
object that cuts inheritance needs nothing more. Otherwise the tree at the ref read gives each
path's nodes; the path's deciding object is found top down exactly as Drive finds it, and only
paths with a cut on that chain are authorized, in one `authorize_batch`. It fails closed: a path
the tree does not hold, a tree that cannot be read, or a repository with more than 10,000 path
objects refuses the path. The operator, and a build's or an app's source credential reading the
revision it was minted for, read every path. Commit messages and authors (`list_changes`,
`get_change`) are the repository's, not a path's, and stay readable to its readers.

**Verify.** `Drive.RepositoryReadsHonourRestrictedFolders`: a repository reader without a grant
on a restricted folder is refused its file, does not see it in the tree and cannot export the
repository; a grant on the folder gives both back.

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
