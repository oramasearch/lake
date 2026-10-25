# Live end-to-end scripts

Scripts that drive a deployed Lake as a person does. They are not part of the native CTest proof.
How to get a Clerk test user and ticket, and what each browser script checks, is
[`docs/deployment/browser-testing.md`](../../docs/deployment/browser-testing.md).

| Script | What it drives |
|---|---|
| `clerk-ticket.sh` | Clerk's Backend API, with `CLERK_SECRET_KEY` from the environment: create a test user, mint a one-time sign-in ticket, delete the user. `CLERK_INSTANCE` refuses any other instance |
| `lib.mjs` | What the browser scripts share: Playwright, the log, ticket sign-in (lake lands on `/`), `/v1` calls with the browser's session, the first top-up on Stripe's test Checkout, the first workspace |
| `walkthrough.mjs` | The React console's Design, Train and Ship as a person does them |
| `mcp.mjs` | Developer → MCP: a read-only key, every client's setup, the skill, a fresh Claude Code session |
| `storage.mjs` | Data → Drives → Storage: each provider Lake can create |
| `services.mjs` | Ship → Services: every service kind |
| `agents.py` | The Agent Library over the agent bus (below) |
| `start-times.py` | How long each service kind takes to start |

## Live agent scenarios
# Browser walkthroughs

`walkthrough.mjs` walks every Design, Train and Ship page after a Clerk ticket sign-in, and
`quickstart.mjs` walks the quickstart process (lake#1128, [docs/ui/quickstart.md](../../docs/ui/quickstart.md)):
the sign-in lands on Quickstart at `/`, a blocked balance is topped up on Stripe's test Checkout
(`lib.ensureFunds`), a CSV is dropped (attached again until the drop zone shows it and *Add it and
build search* is enabled), searched on the page, the plain web page code is served from another
origin and searched there, then step 4's Ask Orama conversation is driven (lake#1182): it waits for
the answer, asks for a model when none was proposed, confirms the `train_quickstart` proposal, checks
the card moved to the training at once and the conversation did not say *Done: train_quickstart*
(lake#1204), and the model's answers appear on the page and on that website. Then a failure path: a
one-row file, clustering asked for and confirmed, and the page must show the reason in plain words
with **Try again** and **Ask Orama to fix it**. `lib.signIn` waits for
`domcontentloaded` and the `quickstart-page` test id, never the `load` event, which lake-dev does
not fire. Both take `<base-url> <ticket-file> <output-dir>`
([docs/deployment/browser-testing.md](../../docs/deployment/browser-testing.md)). `quickstart.mjs`
leaves its dataset, index, model, key and conversation in the account's workspace.

# Live agent scenarios

`agents.py` drives the Agent Library through a deployed Lake's public API. T1–T10 cover reads,
proposals, delegation, agent tasks, and workspace pub/sub (Train → Streams' capabilities). T11
starts Application Agent, validates its proposal, sends the exact object back as
`context.confirmed_proposal`, waits for deployment, and requests the resulting App URL. T12 asks the
Experiments Agent for an experiment, confirms the proposal, waits for trials and requests the
explanation.

```sh
tools/e2e/agents.py <base-url> <token-file> <customer-id> <workspace-id> [scenario ...]
```

Pass the base URL, a file containing a Lake key with every tool, the customer id and the workspace
id (`/v1/viewer` answers both), and optional scenario names. Every call names the workspace. When the
workspace lacks the scenarios' data, the suite uploads `datasets.ux_review_churn` and
`datasets.ux_review_at_risk` first and says so. It changes the live customer only there and in
scenarios whose descriptions say so.
