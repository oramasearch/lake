# Enterprise SSO and SCIM API

## What

An organization administrator registers a SAML 2.0 or OpenID Connect provider with
`register_sso_provider`. A provider has an organization, pool, issuer, attribute mapping, a
just-in-time provisioning choice, and a per-provider SCIM bearer token. SAML providers require
metadata and a signing certificate. OIDC providers require a client id and a client secret.
The client secret is encrypted at rest and the SCIM token is stored only as an Argon2id hash.
Provider reads never return either value. `test_sso_provider` validates the persisted protocol
configuration and records its test time.

`create_federation_issuer` creates one `(organization, issuer, claim)` policy row; a provider
assertion is accepted only when its issuer equals the registered issuer and every asserted claim
has a row for that organization. `sign_in_sso` binds the provider subject to a SCIM user and
issues a browser session. A first assertion may create that SCIM user only when the provider
enables just-in-time provisioning and supplies the user's TAAP identity. The resulting
credential has kind `saml` or `oidc`, and the session remains pool-scoped.

`sign_in_saml` accepts a base64 SAML response only after verifying its enveloped RSA-SHA256
signature and SHA-256 digest with the provider's configured PEM X.509 certificate. The verifier
uses the exclusive-canonicalization subset in Lake's assertion contract and rejects declarations,
entities, and XML forms outside that subset.

`start_sso_oidc` creates an authorization-code flow for an enterprise OIDC provider. It reuses
the identity core's state, PKCE verifier, nonce, one-use callback, and cached JWKS verification.
The callback exchanges the code with the encrypted configured client secret, verifies the ID
token, applies federation issuer policy, and then creates the browser session.

`upsert_scim_user` implements the administrative provisioning capability. The SCIM 2.0 HTTP
surface is rooted at `/scim/v2`: `/Users` and `/Groups` support filtered list, get, create,
PATCH, and delete. Every request authenticates its `Authorization: Bearer` value against the
Argon2id hash stored for one enabled provider; the token identifies the provider and therefore
the target organization. SCIM responses use `application/scim+json`, SCIM resource schemas,
`ListResponse`, and the SCIM error schema.

A user create requires `externalId` or `userName` and the person's TAAP identity. The identity is
accepted as top-level `taapId` or as `taapId` in
`urn:orama:params:scim:schemas:extension:taap:2.0:User`; it binds the provisioned credential to
the durable identity instead of treating the SCIM bearer as that identity. `userName`, the first
email value, `displayName`, and `active` map into the lake user. Setting `active` to false or
deleting the resource revokes every active session and removes SCIM-managed team membership.

A group create maps `externalId` to a stable team handle and `displayName` to the team display
name. Member `value` fields may name the server user id returned by SCIM or the provider's
external user id. PATCH replacement and removal update the source-managed memberships and grants;
group deletion removes the mapped team and its SCIM-managed access. List filters support `eq` on
the returned scalar fields, including `id`, `externalId`, `userName`, and `displayName`, with
`startIndex` and `count` pagination. PATCH supports `add`, `replace`, and `remove`, whole-resource
object values, member-array replacement/addition, and filtered member removal.

The adapter routes call the `scim_list_*`, `scim_get_*`, `scim_upsert_*`, `scim_patch_*`, and
`scim_delete_*` registry capabilities. The older administrative `upsert_scim_user` and
`upsert_scim_group` capabilities remain available to organization administrators.
`list_sso_provisioning_logs` lists the durable create, update, and deactivation outcome log for a
provider.

All configuration, federation policy, sign-in, and provisioning capabilities run through the
registry. They therefore use `authorize` before protected work and append exactly one signed
audit event for every outcome. A rejected issuer is a denied authentication audit event.

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'Sso|Scim|Postgres'
```

The in-memory SSO tests configure one SAML and one OIDC stub provider, cover both successful
assertions, verify a refused unlisted issuer is audited, and exercise SCIM create, update, group
membership replacement, and deactivation with immediate session rejection.
`ScimHttp.RecordedClientSessionProvisionsUsersGroupsAndRevokesAccess` replays the HTTP sequence:
failed bearer authentication, two user creates, filtered list, user PATCH, group create, filtered
member removal, user delete, group delete, and session/access revocation. The Postgres identity
test repeats provider-token authentication and user/group list, get, write, delete, sign-in, and
session revocation. Set `LAKE_TEST_DB_URI` to run its database-backed portion.

## How to change

Keep SSO configuration in `sso_*` tables and federation policy in
`federation_issuers`; extend both `Store` implementations and the capability registry together.
Do not expose a provider secret or SCIM token through JSON. The migration is
`0009_sso_federation`; its numbered guard table is part of the migration contract. The separate
operator repository copies this schema once per wave in its own operator pull request, under the
wave-level waiver; this change updates lake's `db/schema.sql` source of truth only.

Keep SCIM protocol rendering in `src/api/http_server.cpp` and provisioning behavior in registry
capabilities and the store interface. A new SCIM attribute may be preserved in the stored resource
JSON without a migration; a field that changes lake identity or team mapping must be added to both
store implementations and this page.
