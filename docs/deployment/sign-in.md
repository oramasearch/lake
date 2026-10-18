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
| **Without Clerk** (on-prem, air-gapped, [private-install.md](private-install.md)) | Lake's own: password + TOTP, passkeys, TAAP identity, and **Lake OAuth** (Google, GitHub, any OIDC issuer) | Lake's administrator ([§3](#3-lake-oauth-without-clerk)) |
| **A customer organization's own IdP** | **Enterprise SSO**: SAML 2.0 or OIDC, plus SCIM provisioning | The organization's administrator ([§4](#4-enterprise-sso-and-scim)) |

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

Read from `clerk apps list` on 2026-10-18. Applications are addressed by ID; their display names can
change.

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

- `kubectl get lake -A` shows `ClerkReady=True`, with a message like *"production instance on
  lake.live, webhook ep_…, N DNS record(s)"*.
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
`register_oauth_provider` (issuer, client id, client secret, scopes, user pool). Lake encrypts the
client secret under `LAKE_INSTANCE_KEK` ([`../api/identity.md`](../api/identity.md)).

| Provider | Endpoints Lake uses | Redirect URI to register at the provider |
|---|---|---|
| Google | Google's fixed public endpoints. The ID token is verified (RS256, issuer, audience, nonce) | `https://<LAKE_PUBLIC_HOST>/v1/oauth/google/callback` |
| GitHub | GitHub's fixed endpoints. The access token is checked through `https://api.github.com/user` | `https://<LAKE_PUBLIC_HOST>/v1/oauth/github/callback` |
| Any other OIDC issuer | `<issuer>/authorize`, `<issuer>/token` and `<issuer>/jwks`. **Not** OIDC discovery, so an issuer whose endpoints live elsewhere does not work | `https://<LAKE_PUBLIC_HOST>/v1/oauth/<provider>/callback` |

- Plain `http://` provider URLs are refused, except on loopback.
- A first OAuth sign-in may carry the person's TAAP identity document. Lake records it before
  creating the user.
- **Check:** sign in through the provider. The audit log has one `authentication` event for the
  callback.

Password, TOTP, passkeys and TAAP sign-in need no external provider. Passkeys use
`LAKE_PUBLIC_HOST` as the relying-party id, so it must be the host people actually visit.

## 4. Enterprise SSO and SCIM

A customer organization brings its own IdP (Okta, Entra ID, Google Workspace, …). Its administrator
registers it with `register_sso_provider` ([`../api/sso.md`](../api/sso.md)):

| Protocol | The customer gives | Lake gives the customer |
|---|---|---|
| **SAML 2.0** | IdP metadata and its signing certificate (PEM). Assertions must be enveloped and signed with RSA-SHA256 | The ACS URL and entity id (see *Open questions*) |
| **OIDC** | Issuer, client id and client secret (encrypted at rest) | The redirect URI (see *Open questions*) |

Then:

1. Run `create_federation_issuer` once for each accepted claim: one `(organization, issuer, claim)`
   row each. An assertion from an issuer that is not listed is refused and audited as denied.
2. Turn **just-in-time provisioning** on or off. A first sign-in can create the user only when it is
   on and the assertion carries the user's TAAP identity.
3. **SCIM:** Lake creates one bearer token per provider. The customer's IdP calls
   `https://<host>/scim/v2/Users` and `/Groups` with it. Deactivating or deleting a user revokes
   every session of that user at once.
4. **Check:**
   - `test_sso_provider` records a test time;
   - a test user can sign in;
   - `list_sso_provisioning_logs` shows the SCIM create.

## Troubleshooting

| What you see | Cause | Fix |
|---|---|---|
| Google: *Access blocked: Authorization Error · Missing required parameter: client_id* | A social connection is on in a Clerk **production** instance without custom credentials | [§2](#2-social-connections-on-a-clerk-instance), for that provider |
| Google: *Error 400: redirect_uri_mismatch* | The redirect URI at Google differs from the one Clerk shows | Copy Clerk's URI into the OAuth client exactly |
| Google: *Access blocked: … has not completed the Google verification process* | The consent screen is still in *Testing* and the person is not a test user | Publish the consent screen (*In production*) |
| Clerk says sign-ups are restricted, or offers no account | Sign-up is restricted and the email was never invited | Invite the person ([users.md](users.md)). Otherwise this is correct |
| Lake `/sign-in?error=…` mentions the audience or the token | The `lake` JWT template is missing or wrong on the instance | Check `ClerkReady` on the Lake. The operator recreates the template |
| The Clerk widget does not load, or the console reports an origin error | The host is not among the instance's allowed origins | Check `ClerkReady`. The operator adds `https://<host>` and the apex |
| The Clerk dashboard shows DNS not verified on a production instance | The `clerk.`, `accounts.` or email records are missing | Check the Lake's `CloudflareDNSRecord`s, and that `dnsSecretRef` is the zone's token |
| Microsoft sign-in stops working after months | The Entra client secret expired | Create a new secret in Entra and paste it into Clerk |

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
- **Enterprise SSO's URIs** (the SAML ACS URL and entity id, and the OIDC redirect URI) are not stated
  in [`../api/sso.md`](../api/sso.md). Read them from the code before giving them to a customer.
- **Enterprise SSO under Clerk** is not decided: a hosted Lake customer's IdP could connect through
  Lake's own SSO ([§4](#4-enterprise-sso-and-scim)) or through Clerk's enterprise connections.
- **Microsoft client secret expiry dates**, once the secrets are created.
