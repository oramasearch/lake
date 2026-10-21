# Runbook: set up sign-in for every identity provider

How to set up each identity provider a Lake signs people in with, check that it works, and fix the
failures already seen. The API side is [`../api/clerk.md`](../api/clerk.md),
[`../api/identity.md`](../api/identity.md) and [`../api/sso.md`](../api/sso.md); this page is the
deployment procedure.

**Who runs it:** an Orama operator with access to the Clerk dashboard (workspace `steve@orama.com`)
and to the provider consoles (Google Cloud, GitHub, Microsoft Entra, Apple Developer). For a
customer's own IdP, the customer organization's administrator.

**Never commit a credential.** Every client secret, signing secret and Clerk secret key lives in the
provider's console and in a Kubernetes Secret. This page names where it lives, never its value.

## Which provider applies

| Lake | Identity provider | Who configures it |
|---|---|---|
| **Hosted** (`lake-dev`, `lake-testing`, `lake-staging`, `lake.live`) | **Clerk**: email code, and social connections (Google, GitHub, Microsoft, Apple) | The Clerk instance is created once by hand and the operator keeps it configured ([§1](#1-a-clerk-instance)); social connections by hand ([§2](#2-social-connections-on-a-clerk-instance)) |
| **Without Clerk** (on-prem, air-gapped, [private-install.md](private-install.md)) | Lake's own: password + TOTP, passkeys, TAAP identity, and **Lake OAuth** (Google, GitHub, and an OIDC issuer that serves Lake's fixed paths) | Lake's administrator ([§3](#3-lake-oauth-without-clerk)) |
| **A customer organization's own IdP** | **Enterprise SSO**: SAML 2.0 or OIDC, plus SCIM provisioning. **Not ready to offer** ([§4](#4-enterprise-sso-and-scim)) | The organization's administrator ([§4](#4-enterprise-sso-and-scim)) |

Under Clerk, Lake turns its own password sign-up off (`viewer` answers `password_sign_up: false`), so
on a hosted Lake **every person signs in through Clerk**. Clerk sits in front of Lake's own sessions:
Lake verifies a `lake` JWT-template token and issues an ordinary `lake_session`
([`../api/clerk.md`](../api/clerk.md)).

## 1. A Clerk instance

Two Clerk applications serve four environments. Each application's development instance serves the
earlier environment and its production instance the later one:

| Environment | Application | Instance | Frontend API | Secret (managing cluster, namespace `lake`) |
|---|---|---|---|---|
| dev | `lake-testing` (`app_3JpNNpVUwgRgnkItIPbzzRWa2BP`) | development `ins_3JpNNvZd4jnehvW6XsKWcMq6MKx` | `above-seahorse-7535.clerk.accounts.dev` | `clerk-lake-dev` |
| testing | same | production `ins_3Kq0sCu37vKsWjx3jmLzexdO9eK` | `clerk.lake-testing.oramadev.net` | `clerk-lake-testing` |
| staging | `Orama Lake` (`app_3Kq02xRIBqet27addrsP0tGAjTG`) | development `ins_3Kq02xkj1OzPV8CCC9Q2ZB7Ngcq` | `guided-porpoise-2344.clerk.accounts.dev` | `clerk-lake-staging` |
| prod (`lake.live`, `eu.lake.live`, `us.lake.live`) | same | production `ins_3Kq0xJzpNiRB38AIhYHon8uoCYb` | `clerk.lake.live` | `clerk-lake-prod` |

Read from `clerk apps list` on 2026-10-18 and again on 2026-10-19. Applications are addressed by ID;
their display names can change: `app_3JpNNpVUwgRgnkItIPbzzRWa2BP` was first named `lake`, and is now
`lake-testing`.

**What the operator does** with `Lake.spec.clerk` ([orama-operator `docs/lake/clerk.md`](https://github.com/oramasearch/orama-operator/blob/dev/docs/lake/clerk.md)),
from the instance's secret key, every hour and on any change:

- the `lake` JWT template (`aud: lake`, email and name claims, 300 s);
- the Lake's host and apex among the instance's allowed origins (added, never removed);
- the webhook endpoint `https://<host>/identity/clerk-webhook` and its signing secret, given to Lake
  as `LAKE_CLERK_WEBHOOK_SECRET`;
- `LAKE_CLERK_PUBLISHABLE_KEY`, derived from the instance's Frontend API host;
- on a production instance, the DNS records Clerk asks for, as `CloudflareDNSRecord`s in the Lake's
  zone.

**What is done by hand, once per instance.** Clerk's Platform API is a private beta Orama has no
access to.

1. In the Clerk dashboard, in the right workspace (check with `clerk whoami` and `clerk apps list`),
   create the application or the production instance.
2. **User & authentication:** email address on, *verify at sign-up*, sign-in by email code.
3. **Restrictions:** sign-up mode **Restricted**. People are invited ([users.md](users.md)).
4. **Production instance only:** set the domain (`lake.live`, `lake-testing.oramadev.net`). The
   operator writes the DNS records, and the dashboard's DNS check turns green once they resolve.
5. Put the instance's **secret key** (`sk_live_…` or `sk_test_…`, dashboard → API keys) under key
   `secretKey` in the environment's Secret from the table. Creating the Secret is Steve's step.
6. The set member names it, in `config/clusters/sets/lake-*.yaml` of the operator repo:
   `spec.clerk.secretKeySecretRef` and the member's `secrets:` list. Change it by pull request, then
   apply that file.
7. Add the social connections ([§2](#2-social-connections-on-a-clerk-instance)).

**Check:**

- The Lake's `ClerkReady` condition is `True`, with a message like *"production instance on lake.live,
  webhook ep_…, N DNS record(s)"*. `kubectl get lake -A` has no column for it; read it with
  `kubectl describe lake <name> -n <namespace>` (under *Conditions*) or
  `kubectl get lake <name> -n <namespace> -o jsonpath='{.status.conditions[?(@.type=="ClerkReady")]}'`.
- `curl -s https://<host>/v1/viewer` shows `"clerk": {"enabled": true, "frontend_api":
  "clerk.<domain>", "publishable_key": "pk_live_…"}`.
- Sign in with an invited email in a private window, and Lake lands on `/`.

## 2. Social connections on a Clerk instance

**On a development instance**, Clerk's shared credentials work: turn the provider on and nothing else
is needed. **On a production instance, every social connection needs Orama's own OAuth app** (client
id and secret), per Clerk's docs
([Google](https://clerk.com/docs/guides/configure/auth-strategies/social-connections/google),
[overview](https://clerk.com/docs/guides/configure/auth-strategies/social-connections/overview)).
If a provider is turned on without them, sign-in fails at the provider. On `eu.lake.live`, on
2026-10-18, Google answered:

> Access blocked: Authorization Error · Missing required parameter: client_id · Error 400: invalid_request

Each provider follows the same steps:

1. In the Clerk dashboard, on the **production** instance, go to Configure → **SSO connections** →
   the provider → **Use custom credentials**. Copy the **Authorized redirect URI** Clerk shows there
   and use it exactly; never type one from memory. For `lake.live` it is expected to be on
   `https://clerk.lake.live/…`, but that has not been read.
2. Create the OAuth app at the provider (below), with that redirect URI.
3. Paste the client id and secret into Clerk and save. The secret lives only in Clerk and in the
   provider's console.
4. Record who owns the provider-side app, and in which account or project, under *Open questions*
   below.
5. **Check:** in a private window, sign in through the provider with an invited email. With
   restricted sign-up, an address that was never invited is refused by Clerk, which is correct.

Each Clerk **production instance** needs one OAuth app:

- `lake.live`, which serves `lake.live`, `eu.lake.live` and `us.lake.live`;
- `lake-testing.oramadev.net`.

Development instances (dev, staging) need none.

### Google

1. In [Google Cloud Console](https://console.cloud.google.com/), open the project that owns Lake's
   sign-in, then APIs & Services → **OAuth consent screen**:
   - user type **External**;
   - app name *Lake*, a support email, and the authorized domains (`lake.live`, or `oramadev.net`
     for testing);
   - scopes `openid`, `email` and `profile` only;
   - publishing status **In production**. While the screen is in *Testing*, only listed test users
     can sign in (Google allows at most 100), and everyone else gets *Access blocked*.
2. Go to Credentials → **Create OAuth client ID** → *Web application*:
   - **Authorized redirect URIs:** the URI Clerk showed;
   - **Authorized JavaScript origins:** `https://lake.live`, `https://eu.lake.live` and
     `https://us.lake.live` (testing: `https://lake-testing.oramadev.net`).
3. Paste the client id and secret into Clerk (step 3 above).

### GitHub

1. In GitHub, go to the `oramasearch` organization → Settings → Developer settings → **OAuth Apps** →
   New. Set the homepage to `https://lake.live` and the **Authorization callback URL** to the URI
   Clerk showed.
2. Generate a client secret, then paste the client id and secret into Clerk.

### Microsoft

1. In the [Microsoft Entra admin center](https://entra.microsoft.com/), go to App registrations → New.
   Set supported account types to **any organizational directory and personal Microsoft accounts**,
   and the redirect URI (platform *Web*) to the URI Clerk showed.
2. Under Certificates & secrets, create a new client secret. Paste the Application (client) ID and
   the secret's **value** into Clerk.
3. **Record the secret's expiry** (at most 24 months) under *Open questions*. When it lapses,
   Microsoft sign-in stops.

### Apple

1. In Apple Developer → Certificates, Identifiers & Profiles, create:
   - a **Services ID** with *Sign in with Apple*, whose return URL is the URI Clerk showed;
   - a **key** with *Sign in with Apple*.
2. Clerk takes the Services ID, the Team ID, the Key ID and the `.p8` key, and derives the client
   secret from the key.

## 3. Lake OAuth without Clerk

This is for an install with no Clerk publishable key. The administrator registers each provider with
`register_oauth_provider`. Its required inputs are `provider`, `issuer`, `client_id`,
`client_secret` and `scopes`; `pool_id` (the user pool) defaults to `lake`. Lake encrypts the client
secret under `LAKE_INSTANCE_KEK` ([`../api/identity.md`](../api/identity.md)).

**`provider` chooses the endpoints.** Only the exact values `google` and `github` get the fixed
endpoints below; any other value is treated as a generic OIDC issuer (`src/api/oauth.cpp`,
`oauth_endpoints`). The same value is the path segment of the redirect URI.

| `provider` | Endpoints Lake uses | `issuer` | Redirect URI to register at the provider |
|---|---|---|---|
| `google` | Google's fixed public endpoints. The ID token is verified (RS256, issuer, audience, nonce) | Exactly `https://accounts.google.com`: the token's `iss` is compared to it exactly | `https://<LAKE_PUBLIC_HOST>/v1/oauth/google/callback` |
| `github` | GitHub's fixed endpoints. The access token is checked through `https://api.github.com/user` | Required by the input schema, not used for the endpoints | `https://<LAKE_PUBLIC_HOST>/v1/oauth/github/callback` |
| Any other value | Read from `<issuer>/.well-known/openid-configuration`: its `authorization_endpoint`, `token_endpoint` and `jwks_uri` | The issuer, compared exactly to the discovery document's `issuer` and to the ID token's `iss` | `https://<LAKE_PUBLIC_HOST>/v1/oauth/<provider>/callback` |

- **A generic issuer is read through OIDC discovery** ([`../api/identity.md#openid-connect-discovery`](../api/identity.md#openid-connect-discovery),
  lake#837). Microsoft Entra (issuer `https://login.microsoftonline.com/<tenant id>/v2.0`, not the
  multi-tenant `common`), Okta (`https://<org>.okta.com`, or `https://<org>.okta.com/oauth2/<server>`
  for a custom authorization server) and Google Workspace (`https://accounts.google.com`) work. The
  issuer must be exactly the one the provider's discovery document names, trailing slash included.
- Discovery, the token exchange and the keys reach only HTTPS on public addresses. For an identity
  provider on the install's own network, set `LAKE_OIDC_ALLOW_PRIVATE_ADDRESSES=1` (it also allows
  plain HTTP to loopback, for tests). `start_oauth` also needs `LAKE_PUBLIC_HOST`.
- **A first OAuth sign-in must carry the person's TAAP identity document** (`start_oauth`'s
  `identity_document`) with `identity_proof`, a signature by its key over a `begin_identity_proof`
  challenge ([identity.md](../api/identity.md#proving-you-hold-the-key)). Lake verifies and records
  it before creating the user. Without it the user is
  not created, and the callback answers **403** `OAuth identity could not sign in`. A returning
  person, whose provider subject is already linked, does not need it.
- **Check:** sign in through the provider. The audit log has one `authentication` event for the
  callback.

Password, TOTP, passkeys and TAAP sign-in need no external provider. **The passkey relying-party id
is the host of the request's own origin**; `LAKE_PUBLIC_HOST` is used only when the request has no
origin (`webauthn_relying_party` in `src/api/capabilities.cpp`). A passkey registered on one host
therefore does not work on another, for example `eu.lake.live` and `us.lake.live`.

## 4. Enterprise SSO and SCIM

A customer organization brings its own IdP. Its administrator registers it with
`register_sso_provider` ([`../api/sso.md`](../api/sso.md)). Its required inputs are
`organization_id`, `name`, `protocol` (`saml` or `oidc`), `issuer` and `scim_token`.

**Do not offer enterprise SSO to a customer yet.** The gaps below stand in the way. The security
defect that blocked it, [lake#562](https://github.com/oramasearch/lake/issues/562) (the
unauthenticated `sign_in_sso` issued a session for an assertion it never verified), is fixed:
`sign_in_sso` is operator-only.

- **Enterprise OIDC cannot start on Postgres** ([lake#1022](https://github.com/oramasearch/lake/issues/1022)):
  `identity_oauth_flows`' foreign key refuses a flow for an `sso_providers` provider. Its endpoints
  come from OIDC discovery, as in [§3](#3-lake-oauth-without-clerk), so Entra, Okta and Google
  Workspace are supported once that is fixed.

| Protocol | The customer gives | Lake gives the customer |
|---|---|---|
| **SAML 2.0** | IdP metadata (its `entityID` must equal `issuer`, with an HTTP-Redirect `SingleSignOnService`) and its signing certificate (PEM, RSA ≥ 2048 bits). The response or the assertion must be signed (enveloped, exclusive canonicalization, RSA-SHA256 or RSA-SHA512); assertions must not be encrypted | The entity id and metadata URL `https://<LAKE_PUBLIC_HOST>/v1/saml/<provider id>/metadata`, the ACS URL `https://<LAKE_PUBLIC_HOST>/v1/saml/<provider id>/acs` (HTTP-POST), and the sign-on URL `https://<LAKE_PUBLIC_HOST>/v1/saml/<provider id>/start` for the app tile. Lake accepts only responses to its own requests, so the IdP's app must start sign-in at that URL, not post unsolicited responses ([`../api/sso.md#saml-sign-in`](../api/sso.md#saml-sign-in)) |
| **OIDC** | Issuer (exactly the `issuer` its `/.well-known/openid-configuration` names), client id and client secret (encrypted at rest) | The redirect URI `https://<LAKE_PUBLIC_HOST>/v1/oauth/sso-<provider id>/callback`, where the provider id is the one `register_sso_provider` returns |

Then:

1. Run `create_federation_issuer` once for each accepted claim: one `(organization, issuer, claim)`
   row each. An assertion from an issuer that is not listed is refused and audited as denied.
2. Turn **just-in-time provisioning** (`jit_provisioning`) on or off. A first sign-in can create the
   user only when it is on and the sign-in carries the user's TAAP identity (`taap_id` on
   `start_sso_oidc` or `start_sso_saml`). Otherwise the user must already be provisioned through SCIM.
3. **SCIM:** Lake does not create the token. The administrator generates a long random token,
   passes it as `scim_token` to `register_sso_provider`, and gives the same token to the IdP. Lake
   stores only its hash, so it cannot be read back; keep it only in the IdP. The IdP calls
   `https://<host>/scim/v2/Users` and `/Groups` with it as a bearer token. Deactivating or deleting a
   user revokes every session of that user at once.
4. **SCIM users need a TAAP identity.** Map the user's TAAP id in the IdP's SCIM attribute mapping,
   as `taapId` at the top level or as `taapId` in the
   `urn:orama:params:scim:schemas:extension:taap:2.0:User` extension. Without it Lake refuses the
   user: *"SCIM user requires externalId or userName and a TAAP identity"*.
5. **Check:**
   - `test_sso_provider` records a test time;
   - `list_sso_provisioning_logs` shows the SCIM create;
   - OIDC: a test user signs in through `start_sso_oidc`; the provider's issuer is the one its
     discovery document names.
   - SAML: a test user opens `https://<host>/v1/saml/<provider id>/start`, signs in at the IdP and
     lands on Lake signed in. The audit log has an `authentication` event on `saml/<provider id>`
     with outcome `success`; a refused response has outcome `denied` and the reason.

## Troubleshooting

| What you see | Cause | Fix |
|---|---|---|
| Google: *Access blocked: Authorization Error · Missing required parameter: client_id* | A social connection is on in a Clerk **production** instance without custom credentials | [§2](#2-social-connections-on-a-clerk-instance), for that provider |
| Google: *Error 400: redirect_uri_mismatch* | The redirect URI at Google differs from the one Clerk shows | Copy Clerk's URI into the OAuth client exactly |
| Google: *Access blocked: … has not completed the Google verification process* | The consent screen is still in *Testing* and the person is not a test user | Publish the consent screen (*In production*) |
| Clerk says sign-ups are restricted, or offers no account | Sign-up is restricted and the email was never invited | Invite the person ([users.md](users.md)). Otherwise this is correct |
| Lake `/sign-in?error=Your sign-in could not be verified. Please try again.` | Lake refused the Clerk token: wrong audience (the `lake` template's `aud` is not `lake`), wrong issuer, expired, or not signed by the instance's keys. The message names none of these | Check `ClerkReady` on the Lake. The operator rewrites the template. The audit log has an `authentication` event on `clerk` with outcome `denied` |
| The sign-in page shows *Sign-in could not load: …* | Clerk could not give a `lake` template token, for example because the template is missing on the instance. The request never reaches Lake | Check `ClerkReady` on the Lake. The operator recreates the template |
| Lake OAuth callback answers **403** `OAuth identity could not sign in` | A first sign-in without a TAAP identity document ([§3](#3-lake-oauth-without-clerk)) | Start the sign-in with `start_oauth`'s `identity_document` |
| The Clerk widget does not load, or the console reports an origin error | The host is not among the instance's allowed origins | Check `ClerkReady`. The operator adds `https://<host>` and the apex |
| The Clerk dashboard shows DNS not verified on a production instance | The `clerk.`, `accounts.` or email records are missing | Check the Lake's `CloudflareDNSRecord`s, and that `dnsSecretRef` is the zone's token |
| Microsoft sign-in stops working after months | The Entra client secret expired | Create a new secret in Entra and paste it into Clerk |
| SAML ACS answers **403** *unsolicited responses are not accepted* | The IdP posted a response Lake did not ask for (IdP-initiated sign-in from an app tile) | Set the app's sign-on URL (Okta: *Initiate login URI*; Entra: *Sign on URL*) to `https://<host>/v1/saml/<provider id>/start` ([§4](#4-enterprise-sso-and-scim)) |
| SAML ACS answers **403** *for another audience* or *another recipient* | The IdP's audience (SP entity id) or ACS URL is not Lake's for this provider | Copy both from `https://<host>/v1/saml/<provider id>/metadata` |

## Current state (2026-10-18)

- **Google sign-in on `lake.live` fails** with *Missing required parameter: client_id*.
  - `curl -s https://eu.lake.live/v1/viewer` shows `"frontend_api": "clerk.lake.live"` and a
    `pk_live_` key, so the instance is the `Orama Lake` production instance.
  - That instance needs Google custom credentials ([Google](#google)).
  - Email-code sign-in is not affected.
- The `lake-testing` production instance is probably missing the same credentials for every social
  connection that is turned on. This has not been checked.

## Open questions

- **Which social connections are turned on** in each of the four instances, and which have custom
  credentials. This was not read on 2026-10-18, because the configuration read returns client
  secrets. Check it in the dashboard (Configure → SSO connections).
- **Who owns the provider-side OAuth apps:** the Google Cloud project, the GitHub OAuth app, the
  Entra tenant and the Apple team. Record each here once it is created.
- **Enterprise SSO under Clerk** is not decided: a hosted Lake customer's IdP could connect through
  Lake's own SSO ([§4](#4-enterprise-sso-and-scim)) or through Clerk's enterprise connections.
- **Microsoft client secret expiry dates**, once the secrets are created.
