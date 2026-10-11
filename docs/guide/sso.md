# Single Sign-On and SCIM

An organization can connect its own identity provider to Lake: people sign in with their company
account through **SAML 2.0** or **OpenID Connect (OIDC)**, and the provider keeps Lake's users and
teams in step through **SCIM 2.0**. When the provider deactivates someone, Lake ends their sessions
at once.

This page is for organization owners and admins.

## Concepts

| Term | What it is |
|---|---|
| **Provider** | One SAML or OIDC identity provider registered for an organization: its name, protocol, issuer, protocol settings, attribute mapping, whether it may create users on first sign-in, and a SCIM token. |
| **Issuer** | The provider's identifier (the SAML entity or the OIDC issuer URL). An assertion is accepted only from the registered issuer. |
| **Federation issuer claim** | An `(organization, issuer, claim)` row allowing one claim from one issuer. An assertion is accepted only when every claim it asserts is allowed for that organization. |
| **Just-in-time provisioning** | When on, a person's first sign-in may create their Lake user, provided the provider supplies their TAAP identity. |
| **SCIM token** | A bearer secret the provider sends to Lake's SCIM endpoint. It identifies the provider and therefore the organization. |
| **Provisioning log** | The durable record of every SCIM create, update and deactivation for a provider. |

Secrets stay secret: the OIDC client secret is encrypted at rest, the SCIM token is kept only as an
Argon2id hash, and no read ever returns either.

## Open Enterprise SSO

1. Open **Organization → Organizations** and select the organization.
2. Open the **SSO** tab. It lists the providers with their protocol and when each was last tested.
3. Press **Provisioning log** to open the **Enterprise SSO** page, which shows the providers, the
   allowed issuer claims and, for the provider you select, its provisioning log.

- REST: `GET /v1/list_sso_providers?organization_id=<organization id>&limit=100`.
- MCP: `list_sso_providers`.

## Register a Provider

A provider needs, in every case: the organization, a name, the protocol (`saml` or `oidc`), the
issuer and a SCIM token you choose (a long random value; your provider will send it). Then:

- **OIDC** needs the client id and client secret your provider gave Lake.
- **SAML** needs the provider's metadata and its PEM X.509 signing certificate.

The **Add provider** drawer (**Configure SSO Provider**) on the **SSO** tab asks for the **Name**,
**Protocol** (**OpenID Connect** or **SAML 2.0**), **Issuer** and **SCIM token** only, and **Save
provider** sends those. The protocol settings above are required, so register the provider through
the API with them included:

- REST: `POST /v1/register_sso_provider` with
  `{"organization_id": "<organization id>", "name": "Acme Okta", "protocol": "oidc", "issuer": "https://acme.okta.com", "client_id": "<client id>", "client_secret": "<client secret>", "scim_token": "<a long random value>", "jit_provisioning": true}`.
- For SAML, send `"protocol": "saml"` with `metadata` and `signing_certificate` instead of the
  client id and secret.
- `attribute_mapping` (an object) maps the provider's attribute names to Lake's.
- MCP: `register_sso_provider` with the same input.

## Test a Provider

Check that a provider has every setting its protocol needs. Lake records when it was tested; the
**SSO** tab shows *not tested* until then.

- REST: `POST /v1/test_sso_provider` with `{"provider_id": "<provider id>"}`.
- MCP: `test_sso_provider`.

## Allow the Issuer's Claims

Before anyone can sign in, allow each claim the provider will assert, once per claim:

- REST: `POST /v1/create_federation_issuer` with
  `{"organization_id": "<organization id>", "issuer": "https://acme.okta.com", "claim": "email"}`.
- MCP: `create_federation_issuer`.

The **Enterprise SSO** page lists them under **Allowed issuer claims**.

- REST: `GET /v1/list_federation_issuers?organization_id=<organization id>&limit=100`.
- MCP: `list_federation_issuers`.

An assertion from an unregistered issuer, or carrying a claim that is not allowed, is refused, and
the refusal is recorded in the audit log as a denied sign-in.

## How People Sign In

The web app's sign-in page does not show a provider button; an SSO sign-in is started through these
calls, which need no credential:

- **OIDC:** `POST /v1/start_sso_oidc` with `{"provider_id": "<provider id>"}` answers the provider's
  authorization address. The person signs in there, the provider redirects back to Lake, and Lake
  exchanges the code, verifies the ID token, applies the issuer policy and creates the session.
- **SAML:** `POST /v1/sign_in_saml` with
  `{"provider_id": "<provider id>", "saml_response": "<base64 SAML response>"}`. Lake verifies the
  response's RSA-SHA256 signature and digest against the provider's certificate before it issues a
  session.

The session is an ordinary Lake session of kind `saml` or `oidc`. It is bound to a SCIM user; the
first sign-in creates that user only when just-in-time provisioning is on and the provider supplies
the person's TAAP identity.

## Provision Users and Groups with SCIM

Point your provider's SCIM 2.0 client at `https://<your lake>/scim/v2` with the provider's SCIM
token as `Authorization: Bearer <SCIM token>`. Lake serves:

| Resource | Supports |
|---|---|
| `/scim/v2/Users` | list (with `filter` using `eq` on `id`, `externalId`, `userName`, `displayName`; `startIndex` and `count`), get, create, PATCH (`add`, `replace`, `remove`), delete |
| `/scim/v2/Groups` | the same, plus member replacement, addition and filtered removal |

What Lake does with them:

- **A user create** needs `externalId` or `userName`, and the person's TAAP identity as `taapId`
  (top-level, or in the `urn:orama:params:scim:schemas:extension:taap:2.0:User` extension).
  `userName`, the first email, `displayName` and `active` become the Lake user.
- **Setting `active` to false, or deleting the user,** revokes every active session of that person
  and removes their SCIM-managed team memberships.
- **A group** becomes a team: `externalId` is the team's stable handle and `displayName` its display
  name. Members are named by Lake's user id or by your provider's external id. Deleting the group
  removes the team and its SCIM-managed access.

Answers use `application/scim+json`, SCIM's `ListResponse` and SCIM's error schema. To check the
connection, list users from a terminal:

```
curl -s https://lake.oramadev.net/scim/v2/Users \
  -H "Authorization: Bearer $SCIM_TOKEN"
```

An organization admin can also provision one user directly with `upsert_scim_user` or a group with
`upsert_scim_group`.

## Read the Provisioning Log

1. Open the **Enterprise SSO** page (the **Provisioning log** button on the **SSO** tab).
2. Select a provider. **Provisioning log** lists each create, update and deactivation with its
   outcome.

- REST: `GET /v1/list_sso_provisioning_logs?provider_id=<provider id>&limit=100`.
- MCP: `list_sso_provisioning_logs`.

## Permissions

Registering, testing and listing providers, issuer claims, the provisioning log and the
administrative SCIM calls all need `admin` on the organization (an owner or admin). The SCIM
endpoint authenticates with the provider's SCIM token alone, which reaches only that provider's
organization. Every configuration change, sign-in and provisioning call is recorded in the central
audit log.

## Limits

- Protocols: SAML 2.0 (RSA-SHA256 signatures, the exclusive-canonicalization subset Lake verifies)
  and OpenID Connect with PKCE.
- OIDC sign-in needs the Lake to know its public host; without it, `start_sso_oidc` is refused.
- SCIM filters support `eq` only.
- The web app has no SSO button on its sign-in page, and its **Add provider** drawer does not carry
  the protocol settings.

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| *provider configuration is incomplete* | A required field is missing (organization, name, issuer, SCIM token) or the protocol is not `saml` or `oidc`. |
| *provider protocol configuration is incomplete* | OIDC needs `client_id` and `client_secret`; SAML needs `metadata` and `signing_certificate`. Register through the API with them. |
| *SSO provider is not testable* | The same: the protocol settings are missing. |
| A sign-in is refused and the audit shows a denied authentication | The issuer is not the registered one, or a claim is not allowed. Add it with `create_federation_issuer`. |
| SCIM answers 401 | The bearer is not the SCIM token of an enabled provider. |
| A deactivated person can still act | Their sessions end on their next request; an API token they made ends with them. Check **Activity and Audit** for the deactivation. |

## Related Pages

- [Organizations, Members and Teams](/docs/organizations)
- [Your Account and Sign-In](/docs/account)
- [Grants and Effective Access](/docs/access)
- [Activity and Audit](/docs/activity)
- [Security Model](/docs/security)
