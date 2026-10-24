# Testing lake in a browser

How to sign in to a Clerk environment (dev is `https://lake-dev.oramadev.net`) from a browser
without an email code, and walk every page a customer uses. Use it after every roll that changes
something a person sees, and before saying a UI change is done: a green test suite is not evidence
that the real site works. Until the `lake-e2e` image runs the testing gate, these scripts are also
the browser half of the dev → testing gate (the umbrella's `lake-devops` skill).

`lake.oramadev.net` is retired (2026-10-24). Test on `lake-dev.oramadev.net`; never point these
scripts at `lake.live` (the scripts refuse it).

The full manual test of every page, with its personas, test data and cases, is the
[UI test runbook](../ui/test-runbook/README.md). This page is how to sign in for it and the
scripted checks.

## 1. A test user and a one-time sign-in ticket

Clerk issues users and sign-in tokens through its Backend API. Clerk's own sign-in form consumes a
token from the URL (`?__clerk_ticket=`), so the browser signs in through Clerk exactly as a person
does, then lake exchanges the session for its `lake_session`. No email code and no password are
involved.

The dev instance's secret key is in the managing cluster's Secret `lake/clerk-lake-dev` (key
`secretKey`). Read it into the environment of the shell that runs the scripts, never into a file or
a log; the scripts never print it or a ticket:

```sh
export CLERK_SECRET_KEY=$(kubectl --context orama-production-cluster-01 -n lake \
    get secret clerk-lake-dev -o jsonpath='{.data.secretKey}' | base64 -d)
export CLERK_INSTANCE=ins_3JpNNvZd4jnehvW6XsKWcMq6MKx   # dev; the script refuses any other instance

tools/e2e/clerk-ticket.sh create-user "e2e-$(date +%s)+clerk_test@agentcity.app" /tmp/lake-user
tools/e2e/clerk-ticket.sh ticket "$(cat /tmp/lake-user)" /tmp/lake-ticket
```

Use a fresh test user, never someone else's account. Dev's sign-ups are limited to
`*@agentcity.app`, and a `+clerk_test` address is a Clerk test address. A user created this way is
verified, and lake creates its account and personal space the first time it signs in. A ticket lasts
10 minutes and works once; treat it as a password for that account. When you are done, delete the
user and the files:

```sh
tools/e2e/clerk-ticket.sh delete-user "$(cat /tmp/lake-user)"
rm /tmp/lake-user /tmp/lake-ticket; unset CLERK_SECRET_KEY
```

The Clerk CLI is not needed for any of this. Never use these steps against another instance
(testing, staging or lake.live) without the same care: `CLERK_INSTANCE` is the guard.

### A new account's first top-up

lake is prepaid (lake#1114): a new account is blocked until it adds its first $20, and nothing that
costs money starts before that. The scripts add it themselves on Billing → Add Funds, paying on
Stripe's Checkout with Stripe's test card (`4242 4242 4242 4242`). They refuse to pay unless the
Checkout session is in test mode (`cs_test_…`), which dev's Stripe key is. They then start the
account's first workspace from Ask Orama's home at `/ask` (**Start a workspace**) and wait until it
is Ready.

## 2. Sign in by hand

Open `https://lake-dev.oramadev.net/sign-in?__clerk_ticket=<ticket>` in any browser. Clerk signs you
in, and lake lands you on `/`: Quickstart (lake#1128). Ask Orama, *Hello, <first name>*, is at `/ask`.

## 3. The scripts

Every script takes the base URL, the ticket file and an output directory, writes numbered
screenshots and `log.json` there, prints one line per step, and exits non-zero when any check
fails. They use Playwright from the sibling `orama-website-v2` checkout (set `PLAYWRIGHT` to a
`playwright/index.mjs` for another), through `tools/e2e/lib.mjs`.

A ticket works once. To run several scripts on one ticket, set `LAKE_E2E_STATE` to a file path: the
first script saves the signed-in session there (mode 600), and the next ones reuse it. That file is a
credential; delete it when you are done.

The walkthrough signs out at the end. That ends only its own browser session (lake#1163, below), but
that is the session saved in `LAKE_E2E_STATE`, so run it after the scripts that reuse that file. API
keys, `agents.py`'s key and other browsers are not affected. A script that loses its session mid-run
shows *Your session has ended* and fails.

```sh
export LAKE_E2E_STATE=/tmp/lake-session.json
node tools/e2e/mcp.mjs         https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-mcp
node tools/e2e/storage.mjs     https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-storage
node tools/e2e/services.mjs    https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-services
node tools/e2e/walkthrough.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-e2e   # after the scripts sharing LAKE_E2E_STATE: it signs out
rm "$LAKE_E2E_STATE"
```

The scripts find controls by their role and label (the names a screen reader reads), and by a
`data-testid` where a screen has nothing stable: `change-pipeline` (the pipeline page's *Change the
Pipeline* panel), `service-status` (a service's status cell) and `store-status` (a store's status
cell). When you rename a label a script uses, change the script in the same pull request.

### 3a. The walkthrough: Design, Train and Ship

```sh
node tools/e2e/walkthrough.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-e2e
```

| Step | What it checks |
|---|---|
| Sign in | The ticket signs in through Clerk and lake lands on `/` (Quickstart) |
| Funds and workspace | A new account's first top-up on Stripe's test Checkout, then **Start a workspace** on `/ask` and the workspace Ready |
| Design → Projects | **New project** (name, description, decision) opens the project page |
| Build pipeline | **Build pipeline** from what it should predict lists a pipeline under Pipelines |
| Pipeline page | The graph is drawn; *Change the Pipeline* adds an `act` step and an edge to it; **Approve**, then **Check** shows the Checks table |
| Check and run | **Check and run** on the project's approved pipeline starts a run and opens Train → Runs |
| Train → Data, Runs; Ship → Models, Search Indexes, Deployments, Services; Home | Each page shows its title and no error |
| Run report | The first run's **Report** opens its page |
| Search index | **Load data** uploads a small CSV; **New index** over it; a search in the playground has hits; **Publish**; **Create a search key**; `GET /v1/search_index` with that key answers hits, `list_services` with it is refused; **Unpublish** and the outside call is refused; the key is revoked on Tokens |
| Services | Leftover `e2e-` services are deleted; a Postgres service is created, Ready, **Show connection** has a `postgresql://` URI (never logged); Data → Connections **Connect** it; **Load data → From a connection** pulls `SELECT 1`; the service is deleted with its typed name |
| Delete projects | This run's project and any earlier `e2e-` project, with the typed confirmation |
| Account menu → Sign out | lake and Clerk both sign out; `/design/projects` then goes to sign-in |

The sign-out ends only the walkthrough's own browser session (lake#1163): Clerk's session event
names the Clerk session that ended, and lake ends just the Lake browser session bound to it. The
tester's API keys, MCP keys, devices and other browsers keep working, so scripts with their own
browser session or key may run before, after or beside it; only the session saved in
`LAKE_E2E_STATE` ends with it. (Before lake#1163 a Clerk sign-out revoked every session, key and
device of the account.) Keys made in the walkthrough's own browser survive its sign-out too.

Read `log.json` and **look at the screenshots**: a page can show its title and still show an empty
state that should not be empty.

### 3b. The service catalog

```sh
node tools/e2e/services.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-services            # every kind
node tools/e2e/services.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-services kafka sqlite # some
```

Each kind in **New service**'s menu (but `zetta`, the workspace's own) goes through the Services
page: create (1 GiB), wait for Ready, compare the time with the start time the menu names, **Show
connection** (field names only, never values), delete with the typed name, and wait until it is
gone. Postgres is also connected from Data → Connections (**Connect One of Your Services**) and its
tables listed through lake (`list_source_tables`), then the connection is removed.

Our wrapper kinds can also run real queries, each with an escape that must be refused, but only
through `kubectl port-forward`, so this is **off by default** and a gate run (which may only read the
cluster) leaves it off. To run it where port-forwarding is allowed, set `LAKE_E2E_PORT_FORWARD=1`,
`LAKE_KUBE_CONTEXT` and `LAKE_NAMESPACE` (the namespace the services run in):

| Kind | Queries | Must be refused |
|---|---|---|
| `sqlite` | create, insert, select | a batch with a failing statement rolls back whole (400) |
| `duckdb` | create, insert, select | the same batch (400); `read_csv('/etc/passwd')` (403, the lockdown) |
| `polars` | upload an NDJSON dataset, `count` and `sum` over it | `read_csv('/etc/passwd')` (400 `forbidden_sql`, the SQL allowlist) |

Run it after anything that touches a service kind: the lake-service skill (liquid-bridge
`.claude/skills/lake-service`) requires it.

### 3c. Developer → MCP

```sh
node tools/e2e/mcp.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-mcp
```

Opens Developer → MCP from the rail, creates a read-only MCP key, clicks the Claude Code block's
**Copy** and compares the clipboard with the block, checks every client's setup block carries the
key (VS Code asks for it itself), runs `initialize`, `tools/list`, a read call and a write the key
must be refused, fetches the lake skill linked from `llms.txt`, and runs a fresh `claude -p` session
in an empty directory that holds only the skill and an MCP config with the key (one read call,
`server_info`; skipped without the `claude` CLI). It revokes the key under *Your Keys* at the end
and checks it is refused. The key is never printed or screenshotted. Run it after anything that
changes the MCP page, `agent_docs`, or token issuance.

### 3d. Data → Drives → Storage

```sh
node tools/e2e/storage.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-storage
```

Checks that every provider `list_store_providers` marks unavailable has a reason and is left out of
**New storage**'s menu, then for each pair Lake can create without a bucket of the customer's own (a
volume, and the buckets and filesystems Lake keeps) creates one (a 1 GiB volume) on `/storage`,
waits for Ready, shows its connection (field names only), deletes it with the typed confirmation,
and waits until it is gone. The R2, S3, B2, GCS and Azure pairs need the customer's bucket and a
secret mount and are reported as skipped. With `LAKE_KUBE_CONTEXT` and `LAKE_NAMESPACE` set, the
volume's claim is also read with `kubectl get pvc` (read-only). Run it after anything that changes
storage in lake or the operator.

### 3e. The agents

```sh
python3 tools/e2e/agents.py https://lake-dev.oramadev.net /tmp/lake-token <customer-id> <workspace-id> T1 T2 T3
```

Drives the Agent Library through the public API with a key that has every tool (Developer → MCP,
*Every tool*), as [`tools/e2e/README.md`](../../tools/e2e/README.md) describes. Every call names the
workspace. The customer and workspace ids are on `/v1/viewer` (`customer_id`, `workspace_id`).

### 3f. The quickstart process

The quickstart process has its own walkthrough (lake#1128, [../ui/quickstart.md](../ui/quickstart.md)),
with a ticket of its own (a ticket signs in once):

```sh
node tools/e2e/quickstart.mjs https://lake-dev.oramadev.net /tmp/lake-ticket /tmp/lake-quickstart
```

It checks that an account's sign-in lands on Quickstart at `/`, drops a 40-row CSV, searches it on
the page, serves the plain web page code from a local origin and searches there with the key it
made, trains and publishes the model, and checks its answers on the page and on that website. It
stops with the top-up link named when the account's balance is blocked. It leaves the dataset,
index, model and key in the account's workspace.

## What a failure usually means

| Symptom | Where to look |
|---|---|
| Stays on `/sign-in` after the ticket | The ticket expired or was already used, or the Clerk instance does not match the site's `LAKE_CLERK_PUBLISHABLE_KEY` (`../api/clerk.md`) |
| `/sign-in?error=Your sign-in could not be verified` | lake could not verify the `lake` JWT template token: the template's `aud` is not `lake`, or the instance JWKS is unreachable from the pod |
| `funds` fails, or *Add Funds to Start* stays | Stripe's webhook did not reach lake, or the site's Stripe key is not in test mode (the scripts refuse to pay a live Checkout) |
| `workspace` fails: *Preparing your workspace* never ends | The operator's logs for the Lake's data-plane `OramaZettaCluster` (`workspace sync deferred`), and the workspace cluster `<lake>-ws-…` in the Lake namespace |
| *Design, Train and Ship are not available on this Lake yet* | The Lake has neither `spec.services.enabled` nor `spec.workspaces.enabled`, or the operator has not mounted `LAKE_SERVICE_DIR` (`../api/services.md`) |
| An error line starting `operator Zetta returned HTTP 422:` | Zetta refused the control-plane SQL; the text after the colon is Zetta's own reason |
| Build pipeline or run fails with a Zetta message | The message is the workspace's own answer; a new workspace has no data yet, so steps that need a dataset fail until one is added |
| A locator times out | A label or role the script uses changed; the screenshot `failure.png` shows the page. Fix the script in the same change as the label |

## Other environments

- **lake.live**: production. The scripts refuse it unless `LAKE_E2E_ALLOW_PRODUCTION=1` is set, and
  they create and pay for things there; do not run them against it without Steve's go-ahead.
- **Local or on-prem without Clerk**: sign in through lake's own `/sign-in` with a password account
  created at `/sign-up`; the scripts' ticket sign-in does not apply.
