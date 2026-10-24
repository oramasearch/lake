# Runbook: sign-in on a white-labelled Lake

How people sign in to a Lake that serves a customer's own domain (for example `data.example.com`)
without ever leaving it for `lake.live`, `clerk.lake.live` or an Orama page
([lake#1136](https://github.com/oramasearch/lake/issues/1136), part of
[lake#1129](https://github.com/oramasearch/lake/issues/1129)). The domain itself (its DNS, ownership
check and certificate) is the operator's: orama-operator
[`docs/lake/custom-domains.md`](https://github.com/oramasearch/orama-operator/blob/dev/docs/lake/custom-domains.md)
([operator#947](https://github.com/oramasearch/orama-operator/issues/947)). The general sign-in
runbook is [`sign-in.md`](sign-in.md).

**Who runs it:** an Orama operator with access to the Clerk dashboard and the managing cluster's
`config/clusters/` files, and, for the customer's own identity provider, the customer's
administrator. **Never run `clerk` commands from an agent session**; the Clerk steps below are done
by a person in the dashboard. **Never commit a credential**: every key named here lives in a
Kubernetes Secret, never in a file in a repository.

## Choose the model per offer

| Offer | Sign-in | Why |
|---|---|---|
| **Private Warehouse**, **Dedicated Cluster** (hosted by Orama) | **Model A: a Clerk production instance for the customer's domain**, unless the customer requires their own IdP | Email codes, social sign-in and invitations as on `lake.live`, under the customer's domain and name. One instance per white-labelled Lake: a Clerk production instance belongs to one root domain |
| **On-Prem**, or a hosted customer who requires their own IdP | **Model B: the customer's identity provider as the only way in** (SAML 2.0 today; OIDC once [lake#1022](https://github.com/oramasearch/lake/issues/1022) is fixed) | No third party in the sign-in path; people sign in with the account their organization already manages; SCIM deprovisions them |

Lake's own password sign-in (no Clerk, no SSO-only) is also white-label clean, since every page is
Lake's own on the customer's host, but it is not what either offer sells.

## What every model needs first: the domain

1. Add `customDomain.host` to the Lake's member in the operator's `config/clusters/` and apply that
   file (the operator's runbook; Steve's consent for the apply, as for any cluster change).
2. Hand the customer the records in the Lake's `status.customDomain.records`: a `CNAME` and the
   ownership `TXT`, and, for model A, Clerk's records.
3. Wait for `status.customDomain.phase: Routed`. The operator then gives the Lake:

| Variable | Value | What follows it |
|---|---|---|
| `LAKE_PUBLIC_HOST` | `data.example.com` | Every link Lake writes, the OAuth/OIDC callback `https://data.example.com/v1/oauth/<provider>/callback`, the SAML entity id, ACS and sign-on URLs `https://data.example.com/v1/saml/<id>/…`, the passkey relying party `data.example.com`, the console's origin |
| `LAKE_HOST_ALIASES` | the Orama host (`acme.lake.live`) | A browser page opened there is redirected (302) to the same path on `data.example.com`, so a bookmark or an old invitation lands on the customer's domain before any sign-in starts. The API, MCP, git, the registry and webhooks keep answering there unchanged |
| `LAKE_CONTROL_HOST` | the Orama host | The Lake's name on the operator's control rows; never shown to anyone |

The session cookie `lake_session` is host-only (no `Domain` attribute), so it is the customer
host's own; a session started on the Orama host before the switch is not carried over, and people
sign in once more on the customer's domain. Redirects after sign-in are same-origin paths, so they
stay on the host the person is on.

## Model A: a Clerk production instance on the customer's domain

1. **Create the instance (a person, in the Clerk dashboard).** A new application named for the
   customer, its production instance's domain `data.example.com`, so its Frontend API is
   `clerk.data.example.com` and its account pages `accounts.data.example.com`. Under Customization,
   the application's name and logo are the customer's (the email templates and the account portal
   use them); this is where Clerk's own branding is set, not in Lake.
2. **Give the operator its key.** A Secret in the Lake's namespace with `secretKey` (the instance's
   `sk_live_…`), named in the member's `spec.clerk.secretKeySecretRef` (through the set's
   `secrets`). The operator then keeps the instance configured: the `lake` JWT template, allowed
   origins (`https://data.example.com` once its records are verified, and the Orama host), the
   webhook endpoint, and gives the Lake `LAKE_CLERK_PUBLISHABLE_KEY`, `LAKE_CLERK_SECRET_KEY` and
   `LAKE_CLERK_WEBHOOK_SECRET` ([operator `docs/lake/clerk.md`](https://github.com/oramasearch/orama-operator/blob/dev/docs/lake/clerk.md)).
3. **Clerk's DNS records are the customer's.** They are in the customer's zone, where no Orama token
   writes: the operator publishes them in `status.customDomain.records` with purpose `clerk`
   (`clerk.data.example.com`, `accounts.data.example.com`, the mail and DKIM records), and the
   `ClerkReady` condition counts them. Hand them over with the routing and ownership records. Clerk
   verifies them and issues its own certificates for its hosts.
4. **Everything else follows the key.** Lake derives the instance's Frontend API, issuer
   (`https://clerk.data.example.com`) and JWKS from the publishable key
   ([`../api/clerk.md`](../api/clerk.md#configuration)): the token Lake accepts must have that
   issuer, so a token from `lake.live`'s instance is refused there, and the reverse. Nothing in Lake
   names `lake.live`.
5. **Social sign-in** on that instance needs OAuth applications whose consent screen names the
   customer, created in the customer's (or Orama's, named for the customer) Google, Microsoft, GitHub
   and Apple consoles with the redirect URIs Clerk shows for `clerk.data.example.com`
   ([`sign-in.md` §2](sign-in.md#2-social-connections-on-a-clerk-instance) is the procedure; Clerk's
   shared development credentials name Clerk and are never used on production). Leave a provider off
   until its application exists.

**Check:** `curl -s https://data.example.com/sign-in | grep data-clerk-publishable-key` shows a
`pk_live_` key that decodes to `clerk.data.example.com`; sign in with an invited address and land on
`https://data.example.com/` with a `lake_session` cookie for that host; no request in the browser's
network panel goes to a `lake.live` host. Open `https://acme.lake.live/sign-in` and see it redirected
to `https://data.example.com/sign-in`.

## Model B: the customer's identity provider as the only way in

1. **Register the provider** in Lake's own pool, as the customer organization's administrator (or
   the operator with the admin token): `register_sso_provider` with the organization, `protocol:
   saml`, the IdP's `issuer`, metadata and signing certificate, and a SCIM token
   ([`../api/sso.md`](../api/sso.md), [`sign-in.md` §4](sign-in.md#4-enterprise-sso-and-scim)).
   Give the customer's IdP the URLs on **their** host: entity id and metadata
   `https://data.example.com/v1/saml/<id>/metadata`, ACS `https://data.example.com/v1/saml/<id>/acs`,
   sign-on URL `https://data.example.com/v1/saml/<id>/start`. Register it after the host is routed, or
   update the IdP's URLs when it is: they follow `LAKE_PUBLIC_HOST`.
2. **Make it the only way in.** Add `LAKE_SIGN_IN_SSO_PROVIDER=<provider id>` to the member's
   `spec.env` and leave `spec.clerk` unset (Lake refuses to start with both). Then:
   - the sign-in and sign-up pages offer one button, "Continue with <provider name>"; SAML starts at
     the sign-on URL, OIDC through `start_sso_oidc`;
   - in Lake's own pool the registry refuses password sign-up and sign-in (and its MFA step), TAAP
     sign-in, passkey sign-in, social OAuth and email recovery, and any other enterprise provider:
     `unauthorized`, *"this Lake signs people in through its organization's identity provider
     only"*, audited like any refusal;
   - operator sign-in with the admin token stays, for the instance's administrators (`/login`);
   - applications' own user pools are untouched: they are the customer's apps' users, with their own
     settings.
3. **Provision people with SCIM.** Lake creates a user on first SSO sign-in only with just-in-time
   provisioning on **and** a TAAP identity in the sign-in, which a person coming from the customer's
   IdP does not have. Until that gap is closed ([lake#1166](https://github.com/oramasearch/lake/issues/1166)), provision every user through
   SCIM with their TAAP id mapped (`taapId`), as [`sign-in.md` §4](sign-in.md#4-enterprise-sso-and-scim)
   describes. Deactivating a user in the IdP revokes their sessions at once.

**Check:** `viewer` (anonymous) answers `password_sign_up: false` and `sign_in_sso` with the provider
id; `POST /v1/sign_in_password` answers `unauthorized`; the IdP's app tile signs a provisioned user
in and lands on `https://data.example.com/`.

## Roll order

1. **lake** with lake#1136: `LAKE_HOST_ALIASES`, `LAKE_CONTROL_HOST` and `LAKE_SIGN_IN_SSO_PROVIDER`
   are new and change nothing until set. It must reach every environment before step 3: an older
   lake ignores `LAKE_CONTROL_HOST` and would name its control rows by the customer's host.
2. **The operator** with operator#947. No running Lake changes: none has `spec.customDomain`.
3. **The Lake's resource**: `customDomain`, then (model A) `spec.clerk` with the customer's
   instance, or (model B) `LAKE_SIGN_IN_SSO_PROVIDER` in `spec.env`.

## Not covered here

- **Branding** of the sign-in pages, console and email: [lake#1135](https://github.com/oramasearch/lake/issues/1135).
- **Agent surfaces** (`llms.txt`, the skill, `/mcp`) under the customer's name:
  [lake#1137](https://github.com/oramasearch/lake/issues/1137).
- **Clerk satellites** (one instance across several root domains, `LAKE_CLERK_SATELLITES`):
  [orama-operator#636](https://github.com/oramasearch/orama-operator/issues/636); Lake's frontend does
  not load Clerk in satellite mode, so model A is one instance per root domain.
- **The CLI's sign-in** was not checked by lake#1136. Lake's server builds every URL it hands out
  from `LAKE_PUBLIC_HOST`, and the Orama host keeps answering the API, so a CLI configured with either
  host keeps working; a CLI that opens a browser page on the Orama host is redirected to the
  customer's.
