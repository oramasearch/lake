# Testing lake in a browser

How to sign in to a Clerk environment (lake.oramadev.net today) from a browser without an email
code, and walk every page a customer uses. Use it after every roll that changes something a person
sees, and before saying a UI change is done: a green test suite is not evidence that the real site
works.

## 1. Pick a test user

Use a Clerk user that exists for testing, never someone else's account. On dev:

| Email | Clerk user | lake space |
|---|---|---|
| `steve.sperandeo@gmail.com` | `user_3JpUKQFkOXWBelPZAiFWshP0OUH` | `grand-lantern-…` (personal), workspace `ws-6bb777ba498a` |

To add one, follow [`users.md`](users.md). Sign-up is restricted, so a new tester is always created
in Clerk first.

## 2. Mint a one-time sign-in ticket

Clerk issues sign-in tokens through its Backend API. Clerk's own sign-in form consumes one from the
URL (`?__clerk_ticket=`), so the browser signs in through Clerk exactly as a person does, then lake
exchanges the session for its `lake_session`. No email code and no password are involved.

```sh
cd ~/.clerk-lake                      # a directory linked to the Clerk app "lake" (users.md)
/path/to/lake/tools/e2e/clerk-ticket.sh user_3JpUKQFkOXWBelPZAiFWshP0OUH /tmp/lake-ticket
```

The script refuses to run unless the Clerk CLI is linked to `lake`, writes the ticket to the file
with mode 600 and never prints it. A ticket lasts 10 minutes and works once. Treat it as a password
for that account; delete the file when you are done.

## 3a. Sign in by hand

Open `https://lake.oramadev.net/sign-in?__clerk_ticket=<ticket>` in any browser. Clerk signs you
in, and lake lands you on `/repositories`, where your own space is marked *(you)*.

## 3b. Or run the scripted walkthrough

```sh
node tools/e2e/walkthrough.mjs https://lake.oramadev.net /tmp/lake-ticket /tmp/lake-e2e
rm /tmp/lake-ticket
```

It uses Playwright from the sibling `orama-website-v2` checkout (set `PLAYWRIGHT` for another), and
writes a numbered screenshot per step plus `log.json` to the output directory. The steps:

| Step | What it checks |
|---|---|
| Sign in | The ticket signs in through Clerk and lake redirects to `/repositories`; the page marks the viewer's own space |
| Design → Projects | **Start your workspace** is offered, then *Preparing your workspace*, and within a few minutes the project form appears (the workspace is Ready) |
| Create project | A project is created in the customer's workspace and its page opens |
| Build pipeline | The description is captured as the project's intent and compiled into a pipeline |
| Approve | A proposed pipeline is approved (`approve_plan`); Zetta verifies and runs only approved plans |
| Check and run | The pipeline passes `gate` and a run starts; the Runs page lists it |
| Train → Data, Runs; Ship → Models, Search indexes, Deployments; Home | Each page answers 200 with no error line |
| Run report | The first run's page opens with its status. Its report is unavailable on the current workspace image: Zetta's `get_run_report` never answers (https://github.com/oramasearch/zetta/issues/1007), so the page shows *the workspace did not answer get_run_report* after 15 seconds. Expected until that is fixed |
| Account menu → Sign out | lake and Clerk both sign out; `/design/projects` then redirects to sign-in |

Read `log.json` and **look at the screenshots**: a page can answer 200 and still show an error
line or an empty state that should not be empty. The walkthrough creates a project named
`e2e-<time>` in the tester's workspace each run; that is expected.

## What a failure usually means

| Symptom | Where to look |
|---|---|
| Stays on `/sign-in` after the ticket | The ticket expired or was already used, or the Clerk instance does not match the site's `LAKE_CLERK_PUBLISHABLE_KEY` (`../api/clerk.md`) |
| `/sign-in?error=Your sign-in could not be verified` | lake could not verify the `lake` JWT template token: the template's `aud` is not `lake`, or the instance JWKS is unreachable from the pod |
| "Design, Train and Ship are not available on this lake yet" | The Lake has `spec.workspaces.enabled: false`, or the operator has not injected `LAKE_DATA_PLANE_*` / `LAKE_WORKSPACE_*` (`../api/workspaces.md`) |
| An error line starting `operator Zetta returned HTTP 422:` | Zetta refused the control-plane SQL; the text after the colon is Zetta's own reason |
| *Preparing your workspace* never ends | The operator's logs for the Lake's data-plane `OramaZettaCluster` (`workspace sync deferred`), and the workspace cluster `<lake>-ws-…` in the Lake namespace |
| Build pipeline or run fails with a Zetta message | The message is the workspace's own answer; a new workspace has no data yet, so steps that need a dataset fail until one is added |

## Other environments

- **lake.live**: the same, against the production Clerk instance (`clerk-ticket.sh … prod`), once it
  exists ([`users.md`](users.md)).
- **Local or on-prem without Clerk**: sign in through lake's own `/sign-in` with a password account
  created at `/sign-up`; the walkthrough's sign-in step does not apply.
