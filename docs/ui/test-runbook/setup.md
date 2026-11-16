# Setup

Do this once per run, before any section file.

## Environments

| Environment | URL | Use it for | Sign-in | Clerk instance for a ticket |
|---|---|---|---|---|
| testing | `https://lake-testing.example.com` | A version release (the `testing` gate) | Clerk | a Clerk production instance |
| dev | `https://lake-dev.example.com` | Any other run: a change, a roll, a single section | Clerk | a Clerk development instance (`CLERK_INSTANCE=ins_...`) |
| staging | `https://lake-staging.example.com` | Only when a release asks for a staging check | Clerk | a Clerk development instance |
| local | `http://127.0.0.1:4900` (`./build/lake`) | The cases marked *Lake without Clerk*: Lake's own sign-in, sign-up, passkeys, MFA, OAuth | Lake's own pages | none: sign up at `/sign-up` |

Never run this runbook against a production deployment. Your Clerk application and instance ids
come from your own Clerk dashboard.

### Confirm the version

```sh
curl -s https://lake-testing.example.com/v1/server_info | python3 -c 'import json,sys;v=json.load(sys.stdin)["value"];print(v["version"],v["revision"],"billing:",v["billing"],"online_payment:",v["online_payment"],"deployments:",v["deployments"],"region:",v["region"])'
```

- `version` must be the version under test (`0.4.0` for this runbook). On 2026-10-22 every
  deployed Lake still answered `0.3.1`; on those, the cases marked `v0.4.0+` fail by design.
- Write the version, the revision and the four flags at the top of the results file. The flags
  decide which cases apply:

| Flag | When it is off |
|---|---|
| `billing` | The Billing pages show "Not Available" ("This Lake does not bill. Usage is tracked as usual."); run only `BILL-` cases marked *billing off* |
| `online_payment` | No card payment: the payment-method and Stripe cases are `blocked` |
| `deployments` | Ship → Deployments shows "Deployments Are Not Set Up on This Lake"; run only that case |
| `region` | No region menu and no region sentence on sign-in: the region cases are `blocked` |

## Browsers

- A desktop Chromium-based browser at 1440×900, light theme first, then dark.
- A phone viewport of 390×844 (DevTools device mode, iPhone 12 Pro or 13), and a real iPhone and
  Android phone when one is available.
- A second browser profile, or a private window, for a second persona at the same time.
- Keep the browser's developer tools open on the Console and Network panels: the [global
  checks](global-checks.md) read them.
- A passkey-capable browser (a platform authenticator or a security key) for the passkey cases, a
  microphone for dictation, and a TOTP authenticator app for MFA.

## Personas

Each persona is its own Clerk user on the environment (sign-up is restricted, so an admin creates
it in the Clerk dashboard). Give each one an email inbox the team controls, so
invitation and share mail can be read.

| Persona | Who | Prepared state |
|---|---|---|
| `owner` | The main tester | Personal account with two Ready workspaces (`W1`, `W2`); owner of the organization `TD-ORG`; holds all the test data below |
| `member` | A second person | A member (not an administrator) of `TD-ORG`; `read` on `TD-REPO` and on `W1`, nothing else |
| `writer` | A third person, only for the role cases | `write`, not `admin`, on `W1` |
| `newcomer` | A person on their first visit | Signed in once, no workspace, no organization |
| `visitor` | Nobody signed in | A private window |
| `operator` | The instance administrator | Signed in at `/login` with the instance admin token. The token is given by whoever holds it for that run; it is never written in a results file, a screenshot or an issue |

For a persona's first workspace, sign in as that persona and use **Ship → Workspaces → New
workspace** ([04-workspaces.md](04-workspaces.md)); it takes a minute or two to be Ready.

## Signing in

On a Clerk environment, mint a one-time ticket through Clerk's Backend API, with the
environment's secret key in `CLERK_SECRET_KEY` (from wherever your deployment keeps it, never
written to a file) and its instance id in `CLERK_INSTANCE`, and open the sign-in page
with it ([`../../../tools/e2e/README.md`](../../../tools/e2e/README.md)):

```sh
/path/to/lake/tools/e2e/clerk-ticket.sh ticket <clerk user id> /tmp/lake-ticket-owner
```

Open `https://<host>/sign-in?__clerk_ticket=<the file's contents>`. A ticket works once and lasts
10 minutes; delete the file after use. A person can also sign in with the emailed code.

On `local`, sign up at `/sign-up` with a password; the operator signs in at `/login` with the
token Lake printed at start-up.

## Test data

Prepare these as `owner` before the sections that need them. Each section's *Before you start*
names the ones it uses and adds any it alone needs. Data marked *API* has no UI to make it: create
it with the capability named, as `owner`, with an API token from **Account → API Tokens**.

### Accounts and access

| Id | What | How |
|---|---|---|
| `TD-ORG` | An organization `qa-<MMDD>-org`, with `member` as a member and a team `qa-team` | **Organization → Organizations** ([15-organization.md](15-organization.md)) |
| `TD-W2` | `owner`'s second Ready workspace | **Ship → Workspaces → New workspace** |
| `TD-ORGWS` | A workspace owned by `TD-ORG` | **New workspace**, owner `TD-ORG` |
| `TD-9WS` | An account that can see 9 or more workspaces (the picker's filter appears after 8) | Create workspaces, or use an operator-made account |
| `TD-SECRET` | A secret mount in `W1` holding a secret `crm` with a field `token` | **Organization → Secrets** |

### Repositories

| Id | What | How |
|---|---|---|
| `TD-REPO` | A private repository `qa-<MMDD>-repo` with more than 5 changes, a `README.md` with headings, links and raw HTML, `.c` and `.cpp` files, a plain `.txt`, an empty file, a binary, a folder nested two deep and more than 12 top-level paths; one change with a multi-line Markdown message | **New repository**, then `liquid push` |
| `TD-REPO-PUBLIC` | A public repository | *API* `set_repository_visibility` |
| `TD-REPO-EMPTY` | A repository with no refs | **New repository**, push nothing |
| `TD-REPO-TOMB` | A deleted (tombstoned) repository | *API* `delete_repository` |
| `TD-REVIEW` | A review on a change of `TD-REPO`, with a comment on an added line, then approved | *API* `create_review`, `create_review_comment`, `update_review`, or `liquid review` |
| `TD-BUILDS` | Build results on `TD-REPO`: one passed, one failed, one cache hit; a release with attested artifacts | `liquid build`, then *API* `create_release` |
| `TD-WEBHOOK` | A webhook on `TD-REPO` with one failed delivery (an unreachable HTTPS URL) | *API* `create_webhook`, then open an issue |
| `TD-PROTECT` | A branch protection rule (`fast_forward_only`) and one refused push | *API* `set_branch_protection`, then a non-fast-forward `liquid push` |
| `TD-CONFLICT` | A text conflict: one file edited offline twice and both edits pushed | `liquid` |

### Files

Keep them in one local folder, `qa-files/`.

| Id | What |
|---|---|
| `TD-FILES` | One of each: `.md`, `.csv`, `.json`, `.txt`, `.png`, `.jpg`, `.pdf`, `.mp4`, `.mp3`, `.wav`, `.svg`, `.bin`; a folder tree two deep to drag in |
| `TD-BIG` | Files of 5 MiB, 9 MiB and 17 MiB, and a selection over 64 MiB |
| `TD-CSV` | `accounts.csv`, 200 rows: `account_id,plan,seats,monthly_spend,support_tickets,tenure_months,churned`, with `churned` true on 20 to 30% of rows |
| `TD-CSV-WIDE` | `wide.csv`, 1,200 rows (over the Query page's 500-row cap) |
| `TD-CSV-ODD` | `My Data 2024.csv`, `order.csv` (an SQL keyword), `bad.csv` (row 3 has a different field count), `dup-header.csv` (a column named twice), `empty.csv` (0 bytes), `events.jsonl` and `events.ndjson` (20 objects each), `sheet.xlsx` |
| `TD-MODELFILES` | An XGBoost `model.json`, an ONNX tree model `model.onnx`, a LightGBM `model.txt`, a pickle `model.pkl` (must be refused) |
| `TD-AUDIO` | An `.m4a` or `.mp3` recording under 25 MB with speech, and a `.vtt` and a `.txt` transcript under 5 MB |

### Workspace data

| Id | What | How |
|---|---|---|
| `TD-DATASET` | `TD-CSV` loaded into `W1` as `qa_<MMDD>_accounts` | **Data → Datasets → Load data** ([08-data-datasets.md](08-data-datasets.md)) |
| `TD-PROJECT` | A project in `W1` with an approved pipeline that trains a model, run twice: one run succeeded, one failed | **Design → Projects** ([10-design.md](10-design.md)) |
| `TD-MODEL` | A trained model with two versions (from `TD-PROJECT`'s runs) | as above |
| `TD-EXPERIMENT` | An experiment with three or more complete trials | **Train → Experiments**; needs the Experiments Agent |
| `TD-AGENT` | A native agent and a frontier (`runtime: python`) agent in `W1` | **Design → Agents → Agent Factory** |
| `TD-SERVICE` | A Ready Postgres service in `W1` | **Ship → Services** |
| `TD-STORAGE` | A Ready volume and a Ready bucket in `W1` | **Data → Drives → Storage** |
| `TD-PG` | A Postgres reachable from the cluster with a read-only user (host, port, user, password, database) | outside Lake |
| `TD-MCP` | Three MCP servers: public with no auth, one needing a bearer token, one with OAuth; and an MCP package (`@modelcontextprotocol/server-everything`, `mcp-server-fetch`) | outside Lake |
| `TD-IMAGE` | A container image pinned by digest (`name@sha256:<64 hex>`) that serves HTTP | the registry |
| `TD-DEPLOY` | An environment of `TD-REPO` made with `liquid deploy` | `liquid deploy --env staging …` (only where `deployments` is on) |

## Naming

Every object a run creates starts with `qa-<MMDD>-` (`qa-1022-repo`), or `qa_<MMDD>_` where only
letters, digits and underscores are allowed (dataset names). Lake names that must match
`[a-z][a-z0-9-]{0,30}` (services, apps) fit within 31 characters with this prefix. This is how the
clean-up finds what to delete, and the only thing it deletes.

## Clean-up

At the end of the run, as each persona:

1. Delete what the run made, newest first, through the pages that made it: apps, services, search
   indexes, published models (unpublish, then delete), experiments, agents, projects, datasets,
   connections, MCP connections, transcripts, drives and storage, repositories, invitations and
   grants, teams, the organization, the extra workspaces.
2. Revoke the run's API, MCP, predict and search keys on **Account → API Tokens**.
3. Sign every persona out (**Account menu → Sign out**).
4. Delete the ticket files and any downloaded exports.

Anything that cannot be deleted through the UI is listed in the results file for the operator to
remove. Never delete it by hand on the cluster.
