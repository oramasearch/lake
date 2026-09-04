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

`upsert_scim_user` implements SCIM user create, update, and deactivation. Deactivation revokes
every active session for the mapped user. `upsert_scim_group` maps a SCIM group to an
organization team and grants its active mapped users read access. A later group update removes
SCIM-derived access for users omitted from the membership list. `list_sso_provisioning_logs`
lists the durable outcome log for a provider.

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
membership replacement, and deactivation with immediate session rejection. The Postgres identity
test repeats provider registration, federation policy, SCIM user/group writes, sign-in, and
session revocation. Set `LAKE_TEST_DB_URI` to run its database-backed portion.

## How to change

Keep SSO configuration in `sso_*` tables and federation policy in
`federation_issuers`; extend both `Store` implementations and the capability registry together.
Do not expose a provider secret or SCIM token through JSON. The migration is
`0009_sso_federation`; its numbered guard table is part of the migration contract. The separate
operator repository copies this schema once per wave in its own operator pull request, under the
wave-level waiver; this change updates lake's `db/schema.sql` source of truth only.
