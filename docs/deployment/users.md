# Creating users, per environment

How a person gets a lake account in each place lake runs, and how access is taken away. The
sign-in design behind it is [`../api/clerk.md`](../api/clerk.md) (liquid-bridge decisions 43 and
44).

## Which environment uses what

| Environment | URL | Sign-in | Who creates the account |
|---|---|---|---|
| **Dev (AKS)**, `Lake/prod` on `orama-production-cluster-01` | https://lake.oramadev.net | Clerk, application `lake`, **development** instance `above-seahorse-7535` | An Orama admin, in Clerk; sign-up on the site is restricted |
| **lake.live** | https://lake.live | Clerk, application `lake`, **production** instance — **not created yet** | Same flow as dev once the instance exists (below) |
| **Local** (`./build/lake`) | http://127.0.0.1:4900 | lake's own pages, unless started with `--clerk-publishable-key` | The person, at `/sign-up` |
| **On-prem / without Clerk** | the customer's host | lake's own pages | The person, at `/sign-up`, or the customer's SSO |
| **Operator access** (any environment) | `/login` | The instance admin token | Not a user; the token is minted by the operator |

There is one person-facing sign-in per environment. Clerk replaces lake's own pages only where
`LAKE_CLERK_PUBLISHABLE_KEY` is set.

## Dev: lake.oramadev.net

Sign-up is `restricted` in Clerk, so the site will not create an account on its own. An admin adds
the person to Clerk; lake creates their account the first time they sign in.

1. **Confirm the Clerk workspace and application.** The same email can hold several Clerk
   workspaces, and a command run against the wrong one creates the user somewhere lake never looks.

   ```sh
   mkdir -p ~/.clerk-lake && cd ~/.clerk-lake          # any directory outside a repository
   clerk link --app app_3JpNNpVUwgRgnkItIPbzzRWa2BP     # once
   clerk whoami                                          # linked appName must be "lake"
   ```

   If `lake` is not listed by `clerk apps list`, run `clerk auth login` and choose the workspace
   that holds it (created under `steve@orama.com`).

2. **Create the person in Clerk.**

   ```sh
   clerk users create --email person@example.com --first-name Given --last-name Family
   ```

   Clerk marks an address created this way as verified. No password is needed: sign-in is by an
   emailed one-time code.

3. **Tell them to sign in** at https://lake.oramadev.net/sign-in with that email. On first sign-in
   lake either links them to an existing lake account with the same email, or creates the account
   with a lake-held TAAP identity and their personal space. Nothing else needs doing in lake.

4. **Check** (optional): the person lands on `/repositories` with an **Account** menu. Their
   `/sessions` page lists the browser session.

Accounts created so far: `steve@orama.com` (linked to the existing lake account, 2026-09-25) and
`steve.sperandeo@gmail.com` (Clerk user `user_3JpUKQFkOXWBelPZAiFWshP0OUH`, 2026-09-25).

### Letting anyone sign up (not the current setting)

Set the instance's sign-up mode to public and change the invitation wording on the sign-up page
(`web_ui_clerk_sign_in`) in the same change:

```sh
echo '{"auth_access_control": {"sign_up_mode": "public"}}' > patch.json
clerk config patch --file patch.json --yes
```

## lake.live

Not deployed yet (https://github.com/oramasearch/liquid-bridge/issues/17). When it is:

1. Create the **production** instance of the `lake` Clerk application (`clerk deploy`, which
   needs the lake.live domain and its DNS records) and set the same configuration as dev: email
   sign-in with verification, `restricted` sign-up, and the `lake` JWT template with `aud: lake`
   (`docs/api/clerk.md`, Configuration).
2. Set the `pk_live_…` key as `LAKE_CLERK_PUBLISHABLE_KEY` in that `Lake`'s `spec.env`.
3. Create users exactly as for dev, against the production instance: `clerk users --instance prod
   create --email person@example.com` (the `--instance` option belongs to `clerk users`), or the
   Clerk dashboard. Dev and production are separate instances with separate user lists.

Update this page when lake.live exists.

## Local development

`./build/lake --memory-store` has no Clerk, so accounts are created at `/sign-up` with lake's own
form, which asks for a public Liquid identity document:

```sh
liquid identity show --document      # paste the output into the sign-up form
```

To try the Clerk flow locally instead, start lake with the dev instance's key; the development
instance accepts `localhost` origins. Users are the same as dev's:

```sh
LAKE_INSTANCE_KEK=$(openssl rand -hex 32) ./build/lake --memory-store --admin-token local \
  --clerk-publishable-key pk_test_YWJvdmUtc2VhaG9yc2UtNzUzNS5jbGVyay5hY2NvdW50cy5kZXYk
```

A memory store forgets every account when it stops.

## On-prem and other installs without Clerk

Leave `LAKE_CLERK_PUBLISHABLE_KEY` unset. People create accounts at `/sign-up` with an email,
password and their public Liquid identity document (`docs/ui/front-door.md`), or sign in through an
organization's SAML or OIDC provider (`docs/api/sso.md`, with SCIM provisioning). Nothing here
depends on Clerk or on outbound network access.

## Operator access

The operator credential is not a user account. It is the `token` key of the Secret
`lake-<name>-admin` in the Lake's namespace (`lake-prod-admin` for dev), used at `/login`. Treat it
as a secret; never paste it into a ticket or chat.

## Signing in as a tester

To sign in from a browser without an email code, or to walk every page after a roll, use a one-time
Clerk sign-in ticket: [`browser-testing.md`](browser-testing.md).

## Removing access

| Environment | Stop new sign-ins | End existing sessions and tokens |
|---|---|---|
| Clerk (dev, lake.live) | Delete or ban the user in Clerk (`clerk users` or the dashboard) | Operator: `POST /v1/revoke_user_sessions {"user_id": "<lake user id>"}` with the admin token revokes every live session and token; derived tokens stop with their parent. Find the id with `GET /v1/list_users` |
| Without Clerk | Revoke the password credential; SCIM deprovisioning revokes sessions for SSO users (`docs/api/sso.md`) | The same operator capability, or the person on `/sessions` |

Signing out also ends the session on the server, so a copied cookie stops working at once.
