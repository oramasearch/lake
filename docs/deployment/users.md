# Creating users, per environment

How a person gets a lake account in each place lake runs, and how access is taken away. The
sign-in design behind it is [`../api/clerk.md`](../api/clerk.md) (liquid-bridge decisions 43 and
44).

## Which environment uses what

| Environment | URL | Sign-in | Who creates the account |
|---|---|---|---|
| **Dev (AKS)**, `Lake/prod` on `orama-production-cluster-01` | https://lake.oramadev.net | Clerk, application `lake`, **development** instance `above-seahorse-7535` | An Orama admin, in Clerk; sign-up on the site is restricted |
| **lake.live** | https://lake.live (`eu.lake.live`, `us.lake.live`) | Clerk, application `Orama Lake` (`app_3Kq02xRIBqet27addrsP0tGAjTG`), **production** instance `ins_3Kq0xJzpNiRB38AIhYHon8uoCYb`, **sign-up public since 2026-10-24** | The person, at `/sign-up`; an admin can still add people in Clerk ([lake.live](#lakelive) below) |
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
   with a lake-held TAAP identity and their personal account. Nothing else needs doing in lake.

4. **Check** (optional): the person lands on `/` (Ask Orama) with their account menu at the foot of
   the rail. Their
   `/sessions` page lists the browser session.

Accounts created so far: `steve@orama.com` (linked to the existing lake account, 2026-09-25) and
`steve.sperandeo@gmail.com` (Clerk user `user_3JpUKQFkOXWBelPZAiFWshP0OUH`, 2026-09-25) and
`juan@orama.com` (Clerk user `user_3JqKCeuPkKnFflV5QNH6WFQi1kQ`, 2026-09-25).

## lake.live

Sign-up on the production instance is **public since 2026-10-24** (Steve: *"let's drop the signin
restrictions"*, *"it's fine to open up without a paywall"*; set with `clerk config patch`,
`auth_access_control.sign_up_mode: public`). Anyone can create an account at
https://lake.live/sign-up with an email code or a social account; lake creates their account and
personal space on first sign-in.

An admin can still add someone in advance, with the instance's secret key (the managing cluster's
Secret `lake/clerk-lake-prod`, key `secretKey`; never printed): `POST https://api.clerk.com/v1/users`
with `{"email_address": ["person@example.com"], "first_name": ..., "last_name": ...}`, or
`clerk users create` with the CLI linked to `app_3Kq02xRIBqet27addrsP0tGAjTG` and that instance. The
address is marked verified.

A person need not have a name in Clerk. The `lake` template's `name` claim is
`{{user.full_name}}`, which Clerk renders as `null` for a person without one; lake treats a `null`
`name` or `email` as absent (lake#1115: through v0.5.2 such a person could not sign in, and the
sign-in page retried once a second into the rate limit).

Accounts created by an admin so far: `steve.sperandeo@gmail.com` (Clerk user
`user_3L7lEqk8A5xojV3anlcvmoiqaqB`, 2026-10-24, at launch; lake#755).

## What the pages say about sign-up

The web app follows each Clerk instance's own sign-up mode, which clerk-js fetches from the
instance's Frontend API (`userSettings.signUp.mode`, `frontend/src/lib/clerkSignIn.ts`, lake#1116).
There is no second setting in lake to keep in step:

| Mode | Sign-in and sign-up pages | Visitor homepage |
|---|---|---|
| `restricted` or `waitlist` | *Sign in with the email address you were invited with.* and `INVITATION_NOTE` (*Accounts are added by invitation while Lake is in development.*) | Sign in and Read the docs, then `INVITATION_NOTE` |
| `public` | an invitation-free line | **Create an account**, Sign in and Read the docs |
| not known yet (clerk-js loading or unreachable) | the invitation-free line | Sign in and Read the docs |

As of 2026-10-24, clerk-js reports `public` for lake.live and for the dev instance
(`above-seahorse-7535`, whose sign-ups are still limited by its allowlist, `*@agentcity.app`), and
`restricted` for testing and staging. To change who may sign up, change the instance:

```sh
echo '{"auth_access_control": {"sign_up_mode": "public"}}' > patch.json   # or "restricted"
clerk config patch --file patch.json --yes
```

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
