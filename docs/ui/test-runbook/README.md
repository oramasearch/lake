# UI test runbook

A manual test of every page of Lake's web console, written so that a person or an agent with a
browser can follow it from start to finish. It covers the shell, sign-in, every rail group (Home,
Data, Design, Train, Ship, Network, Organization, Administration, Developer) and the account pages,
for each kind of viewer.

It was written from the source of **v0.4.0** (lake `dev` at `225d5c1c`, 2026-10-22). Every label,
message and empty state in quotes is copied from `frontend/src/`. When the app changes, the page
that changed updates its section here in the same pull request (see [Keeping it current](#keeping-it-current)).

It complements the automated checks; it replaces none of them:

| Check | What it proves | Where |
|---|---|---|
| `npm test`, `npm run build` | The app's pure rules (routes, wording, formats) and the effect and list-error checks | `frontend/` |
| `frontend/checks/*.mjs` | Single flows driven by Playwright against a running Lake | `frontend/checks/` |
| `tools/e2e/walkthrough.mjs`, `services.mjs`, `mcp.mjs`, `storage.mjs` | Design, Train and Ship, the service catalog, Developer → MCP and storage on a deployed Lake | [`../../../tools/e2e/README.md`](../../../tools/e2e/README.md) |
| The `testing` e2e gate (`LakeE2ETest`) | Every feature through REST, MCP and a headless browser, for a substantial release | the bridge's `lake-devops` skill |
| **This runbook** | What a person sees and does on every page: labels, states, permissions, phone width, keyboard | here |

## Files

Run them in this order. Each file lists what it needs under *Before you start*.

| File | Cases | Covers |
|---|---|---|
| [setup.md](setup.md) | — | Environments, the version check, test personas, signing in, test data, naming and clean-up |
| [global-checks.md](global-checks.md) | `G-` | The checks that apply to every page, plus phone width, keyboard, theme and permissions across the app |
| [01-shell-and-navigation.md](01-shell-and-navigation.md) | `SHELL-` | Rail, top bar, breadcrumb, workspace picker, command palette, region menu, theme, session ended, error boundary, Not Found |
| [02-sign-in.md](02-sign-in.md) | `AUTH-` | Sign in and sign up (Clerk and Lake's own), operator sign-in, passkey sign-in, MFA |
| [03-home-and-ask-orama.md](03-home-and-ask-orama.md) | `HOME-`, `ASK-` | The landing, Dashboard, the Ask Orama home and side panel, dictation, *From your phone* |
| [04-workspaces.md](04-workspaces.md) | `WS-` | Workspaces, the workspace gate every workspace page shares |
| [05-data-repositories.md](05-data-repositories.md) | `REPO-` | Repositories and every repository page |
| [06-data-drives-and-sharing.md](06-data-drives-and-sharing.md) | `DRV-` | Drives, Storage, a repository's Drive, sharing, Shared with Me, public share pages |
| [07-data-search.md](07-data-search.md) | `SRCH-` | Data → Search |
| [08-data-datasets.md](08-data-datasets.md) | `DSET-` | Datasets, Dataset, Query |
| [09-data-connections-transcripts-mcp.md](09-data-connections-transcripts-mcp.md) | `CONN-`, `TRSC-`, `MCPC-` | Data → Connections, Transcripts, MCP Connections |
| [10-design.md](10-design.md) | `DSGN-` | Projects, Pipeline, Opportunities, Solutions, Methodologies, Agents, Agent Factory, Catalogue Icons |
| [11-train.md](11-train.md) | `TRN-` | Train → Agents, Runs, Experiments, Models |
| [12-train-streams.md](12-train-streams.md) | `STRM-` | Train → Streams, Topic |
| [13-ship.md](13-ship.md) | `SHIP-` | Ship → Models, Search Indexes, Packages, Apps, Deployments, Services, environments, logs, exports |
| [14-network.md](14-network.md) | `NET-` | Network: Overview, Connections, Developer Access, Private Networks, Egress and DNS, Policies, Activity |
| [15-organization.md](15-organization.md) | `ORG-` | Organizations, teams, members, invitations, grants, effective access, enterprise SSO, Secrets, Activity |
| [16-administration.md](16-administration.md) | `ADM-` | The operator's pages: Customers, Applications, Secrets, Encryption Keys, Repository Usage, Audit, Transparency Monitor |
| [17-usage-and-billing.md](17-usage-and-billing.md) | `BILL-` | Usage, Billing, Invoice, the operator's Every Account and Price List |
| [18-developer.md](18-developer.md) | `DEV-` | Developer → API, MCP, Docs |
| [19-account-settings.md](19-account-settings.md) | `ACCT-` | Settings, Sessions, Passkeys, API Tokens, deleting an account |

Each section file ends with **Suspected defects**: things that looked wrong when the section was
written from the code, each with its `file:line`. Confirm or rule out each one while you run that
section, and file the confirmed ones.

## How a case is written

```markdown
#### SHIP-004 · Publish the latest version · P0
As: owner · Needs: TD-MODEL

1. Open **Ship → Models**.
2. On a row whose Serving column says "not published", press **Publish v2**.

Expect:
- The button shows a spinner, then the row's Serving column says "published" with chip `v2`.
- **Call** and **Unpublish** replace **Publish v2** on that row.
```

- **The id** is `<AREA>-<NNN>` and never changes once published. A new case takes the next free
  number in its file. A removed case is deleted, and its number is not reused.
- **Priority:**
  - `P0`: the smoke set. A release does not move on while one fails. Run all of them on every roll.
  - `P1`: core behaviour. Run all of them for a version release.
  - `P2`: edge cases, wording and polish. Run them for a version release, or when the page changed.
- **As** names a persona from [setup.md](setup.md#personas). **Needs** names test data (`TD-…`)
  from [setup.md](setup.md#test-data) or from the file's own *Before you start*.
- **Bold** text is something to press or open, as it is labelled on the page. "Quoted" text is what
  the page must show, character for character. `Code` is a value to type, a URL or a capability.
- **Expect** lists what must be true. Every line must hold for a pass. The [global
  checks](global-checks.md) also hold on every page a case opens, without being repeated.
- `v0.4.0+` after the title marks a case that fails on v0.3.1 and earlier.

## Running it

1. Do [setup.md](setup.md): pick the environment, confirm its version, prepare the personas and
   test data.
2. Start a results file (below), named `<environment>-<version>-<date>.md`, outside the repository
   or in the release's issue.
3. Run [global-checks.md](global-checks.md) once on a few pages to learn what each check looks
   like, then keep it in mind on every page.
4. Run the section files in order. Cases within a file are in an order where each one's data exists
   by the time it is needed; run them top to bottom.
5. Clean up ([setup.md](setup.md#clean-up)).
6. File every failure (below) and write the summary.

For a quick pass after a roll, run only the `P0` cases. `grep -n '· P0$' docs/ui/test-runbook/*.md`
lists them.

### Recording results

```markdown
# Lake UI run: lake-testing, v0.4.0 (225d5c1c), 2026-10-22

Tester: <name or agent> · Browser: Chrome 141, 1440×900 and 390×844 · Personas: owner, reader, newcomer, visitor, operator

| Case | Result | Evidence | Issue |
|---|---|---|---|
| SHELL-001 | pass | | |
| SHIP-004 | fail | shot-ship-004.png: the row still says "not published" after a reload | lake#1101 |
| BILL-012 | blocked | the Lake has `online_payment: false` | |
| DSGN-030 | skip | P2, not run | |

Summary: 412 pass, 6 fail, 9 blocked, 31 skipped.
```

- `pass`: every Expect line held. `fail`: one did not. `blocked`: the case could not be run (state
  why). `skip`: not run by choice.
- A failure's evidence is a screenshot of the page and, when a request failed, the request's path,
  status and response body from the browser's network panel. Never capture a token, a key, a
  password or a connection secret: crop or blur it.

### Filing a failure

File it in `oramasearch/lake` with the label `bug`, one issue per defect:

- **Title:** what is wrong, in the page's words (`Ship → Models: Publish leaves the row "not published"`).
- **Body:** the case id, the environment and its `server_info` version and revision, the persona,
  the steps, what was expected and what happened, and the screenshot.
- Search open issues first. If one exists, add the run's evidence to it instead.

## Rules for the tester

- **Test through the browser only.** Never change a cluster, a database or a Kubernetes object to
  make a case pass or to clean up after one. When a case needs state the UI cannot make, it says so;
  ask for it to be made through the operator.
- **Only on nonprod.** Use `lake-testing.oramadev.net` for a release and `lake-dev.oramadev.net`
  for anything else ([setup.md](setup.md#environments)). Never run this on `lake.live`,
  `eu.lake.live` or `us.lake.live` without Steve's go-ahead, and never on `lake.oramadev.net`,
  which is being removed.
- **Touch only what the run made.** Every object a case creates is named with the run's prefix
  (`qa-<MMDD>-…`); delete only those.
- **Mail and messages to real people:** invitations and shares go only to the test inboxes in
  [setup.md](setup.md#personas).

## Keeping it current

- A pull request that changes what a page shows or does updates that page's cases here: the labels,
  the messages, the steps. It adds a case for a new behaviour and removes the case of a removed one.
- A new page gets cases in the file of its rail group, and a row in the table above if it opens a
  new file.
- When a release is cut, its notes link the run of this runbook that was its proof, and the line at
  the top of this file names the version it was last checked against.
