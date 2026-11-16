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
has a row for that organization. The store's `sign_in_sso` binds the provider subject to a SCIM
user and issues a browser session. A first assertion may create that SCIM user only when the
provider enables just-in-time provisioning and supplies the user's TAAP identity, which must be a
TAAP id ([policy.md](policy.md#principal-ids-lake870)). The resulting credential has kind `saml` or
`oidc`, and the session remains pool-scoped.

**People reach it only through a path that verifies the provider:** the OIDC callback
(`GET /v1/oauth/sso-<id>/callback`) verifies the ID token and calls the store, and `sign_in_saml`
verifies the signed response. **The `sign_in_sso` capability itself is operator-only**
(lake#562): it verifies no assertion, so a caller who is not the admin token is refused, an
anonymous one with `unauthenticated` and a signed-in person with `unauthorized`. Before this it was
a public entry point, and anyone who knew a provider's id, its issuer and a provisioned subject
received that person's session.

SAML providers sign people in through Lake's Assertion Consumer Service; see
[SAML sign-in](#saml-sign-in) below.

**A just-in-time account's identity is Lake's choice** (lake#872). The OIDC callback, the SAML
ACS, `sign_in_saml` and the operator's `sign_in_sso` give the account a provider's first sign-in
creates a managed key Lake mints (`audit::give_managed_key`, kept in `custodial_identities`).
The TAAP id the browser named when it started the flow (`start_sso_oidc`'s document,
`start_sso_saml`'s `taap_id`) or `sign_in_sso`'s `taap_id` is ignored. Before, an unauthenticated
caller chose it, and nothing proved they held its key. A new account also never takes an identity
any user or organization holds (`conflict`).

`start_sso_oidc` creates an authorization-code flow for an enterprise OIDC provider. It reuses
the identity core's state, PKCE verifier, nonce, one-use callback, and cached JWKS verification.
The REST start sets the same per-flow HttpOnly browser cookie as OAuth, and the shared callback
requires its secret before consuming state ([browser binding](identity.md), lake#963). A callback
opened in another browser answers 403 and cannot install a session; the initiating browser can
still finish once.
The provider's endpoints come from its issuer's OpenID Connect discovery document, whose issuer
must equal the registered one ([identity.md](identity.md#openid-connect-discovery), lake#837).
Flow creation validates the synthetic `sso-<provider-id>` callback name against that enabled OIDC
provider before persisting the state.
The callback exchanges the code with the encrypted configured client secret, verifies the ID
token, applies federation issuer policy, and then creates the browser session.

`upsert_scim_user` implements the administrative provisioning capability. The SCIM 2.0 HTTP
surface is rooted at `/scim/v2`: `/Users` and `/Groups` support filtered list, get, create,
PATCH, and delete. Every request authenticates its `Authorization: Bearer` value against the
Argon2id hash stored for one enabled provider; the token identifies the provider and therefore
the target organization. SCIM responses use `application/scim+json`, SCIM resource schemas,
`ListResponse`, and the SCIM error schema.

A user create requires `externalId` or `userName`. **Lake chooses the account's TAAP identity**
(lake#872): every account a SCIM create makes gets a managed key Lake mints, as a Clerk or password
sign-up does. A `taapId`, top-level or in `urn:orama:params:scim:schemas:extension:taap:2.0:User`,
is dropped: it is neither used nor stored nor answered. Before, the SCIM client named it, so an
organization's provider could give its user the identity of another organization, of a person who
had not signed up yet, or of anyone a grant named, and that user inherited the grants. `userName`, the first
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

## SAML sign-in

Lake is the SAML 2.0 service provider (Web Browser SSO profile, lake#836). For a provider with id
`<id>` on a Lake whose `LAKE_PUBLIC_HOST` is `<host>`:

| What | Where |
|---|---|
| SP entity id, and the URL of Lake's metadata (`application/samlmetadata+xml`) | `GET https://<host>/v1/saml/<id>/metadata` |
| Assertion Consumer Service, HTTP-POST binding | `POST https://<host>/v1/saml/<id>/acs` |
| Sign-on URL: starts sign-in and redirects to the IdP (the app tile's URL) | `GET https://<host>/v1/saml/<id>/start[?return_to=/path]` |

All three need no credential and answer `404` for a provider that is not an enabled SAML provider.
The metadata says `WantAssertionsSigned="true"` and `AuthnRequestsSigned="false"`.

**The flow.** `start_sso_saml` (`{provider_id, device_name?, taap_id?, return_to?}`, no credential;
`taap_id` is accepted and ignored, lake#872)
or the start route records a one-use AuthnRequest for ten minutes and answers the IdP's sign-in URL:
the provider metadata's HTTP-Redirect `SingleSignOnService` (HTTPS, or loopback for tests), with
the request DEFLATE-encoded in `SAMLRequest`. The metadata's `entityID` must equal the provider's
`issuer`. The IdP posts its `SAMLResponse` to the ACS. Lake verifies it (below), consumes the request
it answers, records the assertion, runs `sign_in_sso`'s provisioning (federation issuer policy, the
SCIM user or just-in-time provisioning with a managed key Lake mints), sets `lake_session` and
redirects to `return_to` (a same-origin path) or `/`. `RelayState` is not used. Every outcome is
one `authentication` audit event on `saml/<id>` (method `saml`; outcome `success`, `denied` with the
reason, or `failure`). The ACS takes the IdP's cross-site form post, so it is exempt from the
[cross-site check](../serving/http.md#cross-site-requests); it never reads the session a cookie
carries, it replaces it.

`sign_in_saml` (`{provider_id, saml_response, taap_id?, device_name?}`, no credential; `taap_id`
ignored) is the same
verification through the registry, for a client that relays the response itself; it answers the
session instead of a cookie. Both refuse a response that answers no outstanding request.

**What a response must be.** Verification is Lake's own (`src/control/saml.cpp`, no XML library;
the bridge's *We write our own* rule and decision 32), and it fails closed:

- **XML.** A strict reader: one root, an optional XML declaration, comments (dropped), the five
  predefined entities and character references. DTDs, other entities, CDATA and processing
  instructions are refused, as are unbound prefixes, repeated attributes, more than 64 levels or
  20,000 elements, and more than 1 MiB.
- **Signature.** The `samlp:Response`, the one `saml:Assertion` it carries, or both, carry an
  enveloped XML Signature as a direct child, verified with the provider's registered certificate
  (PEM, or bare base64 DER; RSA, at least 2048 bits). The signature's `KeyInfo` is ignored. Each
  present signature must verify, and at least one must cover the assertion. A signature has exactly
  one `Reference`, whose `URI` is `#` and the ID of the element it sits in, an ID no other element
  in the document uses; its transforms are exactly enveloped-signature then Exclusive XML
  Canonicalization 1.0 without comments (with an optional `InclusiveNamespaces` `PrefixList`); its
  digest is SHA-256 or SHA-512; `SignedInfo` is canonicalized the same way and signed with
  RSA-SHA256 or RSA-SHA512. Anything else (SHA-1, other transforms, more references, signatures in
  any other place) is refused.
- **Against signature wrapping**, the response must carry exactly one assertion anywhere in the
  document, as its direct child; no element may share the signed ID; and every value Lake uses is
  read from that verified assertion by direct child. Text split by a comment is read whole, as it
  was signed.
- **Status** `Success`. `Destination`, when present, is the ACS URL. The response's `Issuer`, when
  present, and the assertion's are the provider's `issuer`. Encrypted assertions are refused.
- **Subject.** A `NameID` (the provider subject). A bearer `SubjectConfirmation` whose
  `SubjectConfirmationData` has `Recipient` equal to the ACS URL, a `NotOnOrAfter` still ahead, no
  `NotBefore` still ahead, and an `InResponseTo` naming an outstanding request (equal to the
  response's `InResponseTo` when that is present). Unsolicited, IdP-initiated responses are refused.
- **Conditions.** `NotBefore` and `NotOnOrAfter` hold, and at least one `AudienceRestriction`, each
  of which names the SP entity id. Conditions other than `OneTimeUse` and `ProxyRestriction` are
  refused. Times allow two minutes of clock skew.
- **An `AuthnStatement`**, whose `SessionNotOnOrAfter`, when present, is still ahead.
- **Once.** The request is consumed and the assertion's ID recorded until it expires, in one
  transaction; a second use of either is refused.

The email is the attribute the provider's `attribute_mapping` names for `email`, else `email`,
`mail`, the Entra claim `…/claims/emailaddress` or `urn:oid:0.9.2342.19200300.100.1.3`; the display
name likewise from `display_name` or `name` (`name`, `displayName`, Entra's `…/claims/displayname`,
`urn:oid:2.16.840.1.113730.3.1.241`). As for OIDC, the claim the issuer policy checks is `email`
alone, when present. Not supported: encrypted assertions, signed AuthnRequests, IdP-initiated
sign-in and single logout.

**Storage.** Migration `0092_saml_sign_in` adds `sso_saml_requests` (the request id only as a
SHA-256 hash, the device name, TAAP id and return path, ten-minute expiry, `used_at`) and
`sso_saml_assertions` (provider and assertion-id hash, kept until the assertion's `NotOnOrAfter`),
both cascading with their provider, and the guard `saml_sign_in_guard`
([schema.md](../control-plane/schema.md#saml-sign-in-migration)).

## The customer's provider as the only way in

**lake#1136.** A white-labelled Lake can make one enterprise provider the only way in for people:
`LAKE_SIGN_IN_SSO_PROVIDER=<provider id>`. It is an instance setting the operator renders from the
Lake's `spec.env`; startup stops when it is not a provider id, or when a Clerk publishable key is
also set (a Lake signs in through Clerk or through the customer's provider, never both).

- **The registry refuses the rest** before any handler, one gate for every entry point
  (`gate_instance_sign_in` in `src/api/registry.cpp`, `sso_only_refusal` in `src/api/white_label.cpp`):
  in Lake's own pool (`pool_id` `lake` or absent) `sign_up_password`, `sign_in_password`,
  `verify_mfa`, `sign_in_taap`, `begin_passkey_sign_in`, `sign_in_passkey`, `start_oauth` and
  `begin_email_recovery` answer `unauthorized`, *"this Lake signs people in through its
  organization's identity provider only"*. `start_sso_oidc`, `start_sso_saml` and `sign_in_saml`
  are refused for any provider of Lake's own pool but the configured one; the SAML sign-on route
  `GET /v1/saml/<id>/start` gives the same refusal. Each refusal is audited like any other.
- **Not affected:** operator sign-in with the admin token (the instance's administrators), and
  applications' own pools, whose users and sign-in methods are the customer's applications'.
- **`viewer`** answers `password_sign_up: false` and `sign_in_sso: {provider_id, protocol, name}`
  (null without the setting). The web app's sign-in and sign-up pages then offer one button, named
  for the provider (`frontend/src/pages/account/SsoSignIn.tsx`): SAML opens the sign-on URL with the
  page's same-origin `return_to`, OIDC calls `start_sso_oidc`.
- **Users must be provisioned through SCIM for now.** A first sign-in creates a user only with
  just-in-time provisioning and a TAAP id in the sign-in, which a person from the customer's IdP
  does not carry. Minting a managed key for such a person, as a Clerk sign-in does (decision 65), is
  [lake#1166](https://github.com/oramasearch/lake/issues/1166). Enterprise OIDC also waits for
  [lake#1022](https://github.com/oramasearch/lake/issues/1022) on Postgres; SAML works.

The runbook, with the Clerk alternative, is
[`../deployment/white-label-sign-in.md`](../deployment/white-label-sign-in.md). Tests:
`WhiteLabelSsoOnly.*` in `tests/test_white_label.cpp`, `frontend/tests/sso-sign-in.test.ts`.

## How to verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure -R 'Saml|Sso|Scim|Postgres'
```

For the release proof, create a fresh database, apply the complete schema, and run the entire
suite so the SSO migration is exercised with every other control-plane migration:

```sh
docker exec lake-test-pg-18 psql -U postgres -c 'CREATE DATABASE lake_sso_test'
docker exec -i lake-test-pg-18 psql -U postgres -d lake_sso_test < db/schema.sql
LAKE_TEST_DB_URI=postgresql://postgres:lake@127.0.0.1:55432/lake_sso_test \
  LAKE_INSTANCE_KEK=<64-hex-key> \
  ctest --test-dir build --output-on-failure
```

The in-memory SSO tests configure one SAML and one OIDC stub provider, cover both successful
assertions, verify a refused unlisted issuer is audited, and exercise SCIM create, update, group
membership replacement, and deactivation with immediate session rejection.

`tests/test_saml.cpp` covers SAML sign-in: Exclusive Canonicalization against the specification's
examples and libxml2's output; Okta- and Entra-shaped responses signed by an independent
implementation (`tests/fixtures/saml`, signed by signxml); tampered, re-signed-with-another-key,
SHA-1, XPath-transform, unsigned, encrypted, unsolicited, wrong-audience, wrong-recipient,
wrong-destination, wrong-issuer, expired and not-yet-valid responses; five signature-wrapping
attacks and a comment inside the `NameID`; the AuthnRequest redirect and both metadata documents;
the ACS, start and metadata routes end to end (sign-in once, replay of the response and of the
assertion refused, audit); and the store's once-only request and assertion records in memory and
on Postgres. `Sso.SignedSamlResponseIsVerifiedBeforeSignIn` runs `sign_in_saml` through the
registry.
`ScimHttp.RecordedClientSessionProvisionsUsersGroupsAndRevokesAccess` replays the HTTP sequence:
failed bearer authentication, two user creates, filtered list, user PATCH, group create, filtered
member removal, user delete, group delete, and session/access revocation. The Postgres identity
test repeats provider-token authentication and user/group list, get, write, delete, sign-in, and
session revocation. Set `LAKE_TEST_DB_URI` to run its database-backed portion.

## How to change

Keep SSO configuration in `sso_*` tables and federation policy in
`federation_issuers`; extend both `Store` implementations and the capability registry together.
Do not expose a provider secret or SCIM token through JSON. The migration is
`0010_sso_federation`; its numbered guard table is part of the migration contract. The separate
operator repository copies this schema once per wave in its own operator pull request, under the
wave-level waiver; this change updates lake's `db/schema.sql` source of truth only.

Keep SCIM protocol rendering in `src/api/http_server.cpp` and provisioning behavior in registry
capabilities and the store interface. A new SCIM attribute may be preserved in the stored resource
JSON without a migration; a field that changes lake identity or team mapping must be added to both
store implementations and this page.
