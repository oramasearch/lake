# Customer entry and navigation

Status: checked on 2026-10-01 against lake `36679e1` on the dev cluster (lake.oramadev.net, Clerk
sign-in), and pinned by `WebUi.FrontDoorDocQuotesTheRenderedHomePage`.

The homepage says what lake is for: designing, training and shipping AI on the customer's own
data, in a workspace of their own. It uses the existing design-system shell, panels, grid, and
theme controls. The earlier repository and `liquid push` homepage (2026-09-08, below under
*Browser evidence*) no longer exists; repositories are one link under Ship.

**Sign-in.** When `LAKE_CLERK_PUBLISHABLE_KEY` is set, Sign in opens Clerk's form and lake's own
password, TOTP, passkey and identity-document pages leave the navigation; see
[`../api/clerk.md`](../api/clerk.md). Clerk's sign-up is restricted while lake is in development,
so a visitor is offered **Sign in** only, never Create account, and the homepage and `/sign-up`
say the same sentence about invitations (`web_ui_invitation_note`). Without Clerk, which is what an
on-prem install uses, password sign-up is open and the visitor is offered **Create account** too.

## Landing copy

Every bold phrase in this section is quoted from `web_ui_home` (`src/api/web_ui.cpp`); the test
named above fails when one stops appearing on the rendered page.

**lake by Orama**

**Design, train, and ship AI on your own data.**

**Load your data into your own workspace, train models on it, and call them from your code, in
Orama's cloud or your own.**

The visitor actions are **Sign in** and **Read the docs**. Under Clerk they are followed by
**Accounts are added by invitation while lake is in development.** Without Clerk the first action
is **Create account**. A signed-in account instead gets **Getting started** (the Docs) and
**Load your data**.

Three cards follow, one per verb. Each states only what lake does today:

1. **Design**. **Start from the business question and the decision it should improve. A project
   turns it into a pipeline you can read, change and approve.** Signed in, it links Projects.
2. **Train**. **Your data lives in your workspace: upload a file, connect a database or generate a
   dataset, then train on it and read what each run found.** Signed in, it links Data and Runs.
3. **Ship**. **Publish a model and call it from your code, publish a search index, or run
   databases for your team. Files and code live in repositories.** Signed in, it links Models and
   Repositories, and an ordinary account also gets **Create a token**.

## Navigation

| Viewer | Sidebar | Top bar |
|---|---|---|
| Signed out or invalid credential | Home, Public Repositories · **Developer:** Docs | Sign in; also Create account when Clerk is off |
| Ordinary account | Home, Repositories, Shared with Me, Search · **Design:** Projects, Opportunities, Solutions, Methodologies, Agent Library · **Train:** Agents, Data, Runs, Experiments, Streams · **Ship:** Models, Search Indexes, Packages, Apps, Deployments, Services, Workspaces · **Organization:** Organizations, Secrets, Usage (opens on the account's own space), Activity (`/activity`, the account's own audit events via `my_audit`: newest first, principals by name, a workspace call's inner capability, successful reads hidden unless **Include reads** is ticked; see [audit.md](../api/audit.md#reading-the-log-as-a-person-lake445)) · **Developer:** API, MCP, Docs | Space and workspace selectors, Ask Orama, **Account** menu: Settings, Sessions, Passkeys (omitted when Clerk is on), Tokens, Sign out |
| Operator | Home, Repositories, Search · **Design** · **Train** · **Ship** (as an account) · **Administration:** Customers, Organizations, Applications, Usage, Secrets, Audit · **Developer:** API, MCP, Docs | Sign out |

**Design, Train and Ship** run on the workspace selected in the top bar, one of the space's
workspace Zettas; see
[`../api/workspaces.md`](../api/workspaces.md). Their pages live under `/design`, `/train` and
`/ship`. The repositories list and the create-repository form mark the viewer's own personal space
with *(you)* and *your personal space*, since its generated handle is otherwise unrecognisable.

**The current item is chosen by request path.** `context_for` records the route path on
`ApiContext::request_path`, `viewer_for` copies it into `WebUiViewer::path`, and `nav` compares
it against each item's routes: an exact match, or a parent (`/drive` owns `/drive/preview`, and a
repository's own `/environments` belongs to Repositories). A renderer called without a request, as
in the unit tests, falls back to matching the page title. The path is never used to authorize.

The account menu is a `<details>` element, so it needs no script. Operators have no personal pages
and get Sign out alone.

**Deployments** (`GET /deployments`) lists every environment across the repositories the caller may
list, from the same `list_repositories` and `list_environments` capabilities the per-repository
pages call. A repository whose environments the caller may not read is skipped. Signed-out
requests redirect to `/sign-in`.

Inside a repository, the first tab is **Files** (the drive view) for signed-in viewers, then
**Code**. The tab formerly labelled Platform is **Deployments**.

Before 2026-09-08, every viewer saw Home, Repositories, Shared with me, Search, Customers, Organizations,
Applications, Usage, Secrets, Audit, and API, plus an implementation label in the sidebar footer.
The footer is removed. The public repository list is filtered by the existing registry policy.

Repository tabs also omit Platform and Webhooks for anonymous readers. Protection requires the
instance audit read and is offered only to operators. Anonymous repository homes omit the access
details link. Direct requests still use the existing capability authorization.

`WebUiViewer` carries authenticated and operator state from the common `context_for` resolver
through every shared-shell renderer, including error and public pages. Renderers remain pure.
Where a handler has already resolved its context, it supplies that context's viewer; there is no
second authorization mechanism. Boolean renderer arguments remain source compatible and identify
an ordinary account. Application-specific hosted identity pages retain their separate themed shell.

Customer authentication redirects lead to `/sign-in`. `/login` remains a direct, explicitly named
operator entry with the existing `POST /session` contract. Its copy points account holders to their
own sign-in. A wrong operator token is recorded as a failed sign-in and redirects to
`/login?error=That%20token%20did%20not%20match.`, which `/login` shows (lake#561: the web app reads
`error` the same way); before, the form answered `401` with the page. OpenAPI now describes account tokens and public operations accurately.

## Verify and change

Run the complete native static and shared suites with distinct fresh PostgreSQL databases,
`LAKE_TEST_DB_URI`, and `LAKE_INSTANCE_KEK`, as described in [the test contract](../build/testing.md).
`WebUi` tests pin the explanation, ordinary credential path, and viewer navigation. The parameterized
HTTP navigation test covers both stores, absent and invalid cookies, user sessions, operator
credentials, cross-page consistency, and HTTP 200 for every offered global destination. Its
PostgreSQL variant creates a UUID-named schema, loads `db/schema.sql` there, uses that schema in
its connection URI, and removes it after each case. Its audit records cannot enter another test's
audit window.

`PostgresCapabilities.OrganizationAndPolicyRoundTrip` gives a repository deletion the audit object
`repository/<id>` and asks the `audit` capability for that exact successful record. It does not rely
on a bounded page of unrelated audit events. The capability registry remains the shared source for
REST, OpenAPI, and MCP.

To change navigation, update `nav` and `forge_tabs` in `src/api/web_ui.cpp` together with this
matrix and the HTTP assertions. New data operations must still be capabilities in the registry.
Do not infer operator status from a page name, a nonempty cookie, or a customer's repository role.

For browser verification of an install without Clerk, start the native server on a separate fresh
database. Open a new profile at each of 1280 and 640 pixels in light and dark, follow the homepage's
Create account action,
export a fresh public Liquid identity document, submit the form, and create the first private
repository. Save the rendered customer pages and check their text for implementation names and
operator-credential instructions. The recorded browser evidence follows.

## Browser evidence

Historical: this records the 2026-09-08 repository homepage, which has since been replaced by the
landing copy above. It is kept as the evidence of that change.

Recorded on 2026-09-08 against the native server at `http://127.0.0.1:4935`, using a separate
`lake_walkthrough_135` PostgreSQL 17.7 database. Local Chrome completed the journey in four fresh
contexts. The in-app browser connection was unavailable at startup.

The signed-out navigation list was identical in all four profiles: **Home, Public repositories,
Sign in, Create account**. The brand also links Home, and the main content repeats Create account
and Sign in. The theme control and skip link remain available. No operator destination appears.
Every visitor and ordinary-account global navigation destination returned HTTP 200 with the same
profile.

Each profile followed Create account from the homepage, ran `liquid identity show --document`
with its own `LIQUID_HOME`, entered that public document and ordinary account fields, and landed
on the repository creation form with its generated personal handle selected. Creating the named
repository opened its home showing private visibility and inherited `admin` access. Document width
equaled viewport width on every captured page. The narrow account navigation retains its existing
horizontal scrolling.

| Profile | Personal handle | Repository | Access |
|---|---|---|---|
| 1280-light | `quick-delta-385` | `first-repository-1280-light` | private, inherited admin |
| 1280-dark | `olive-vista-744` | `first-repository-1280-dark` | private, inherited admin |
| 640-light | `gentle-vista-158` | `first-repository-640-light` | private, inherited admin |
| 640-dark | `fair-cedar-262` | `first-repository-640-dark` | private, inherited admin |

| Profile | Landing | Sign-up | Repository form | First repository |
|---|---|---|---|---|
| 1280-light | [Landing](screenshots/front-door/1280-light-home.png) | [Sign-up](screenshots/front-door/1280-light-sign-up.png) | [Form](screenshots/front-door/1280-light-repositories.png) | [Repository](screenshots/front-door/1280-light-repository.png) |
| 1280-dark | [Landing](screenshots/front-door/1280-dark-home.png) | [Sign-up](screenshots/front-door/1280-dark-sign-up.png) | [Form](screenshots/front-door/1280-dark-repositories.png) | [Repository](screenshots/front-door/1280-dark-repository.png) |
| 640-light | [Landing](screenshots/front-door/640-light-home.png) | [Sign-up](screenshots/front-door/640-light-sign-up.png) | [Form](screenshots/front-door/640-light-repositories.png) | [Repository](screenshots/front-door/640-light-repository.png) |
| 640-dark | [Landing](screenshots/front-door/640-dark-home.png) | [Sign-up](screenshots/front-door/640-dark-sign-up.png) | [Form](screenshots/front-door/640-dark-repositories.png) | [Repository](screenshots/front-door/640-dark-repository.png) |

The first desktop account also created a named token through **Tokens**, with the displayed sync
scopes and a 3600-second expiry. Using the same local identity, `liquid init .`,
`liquid cut "First version"`, and `liquid push` published a README. The repository page then
rendered `Code and files stored in Lake.`. The final push reported:

```text
metadata_items=4
objects_sent=1
bytes_sent=661
objects_received=0
bytes_received=0
cursor=1
reference=main outcome=applied
```

The initial `init` imports files; `cut` names the change before publication. The push proof used
the running local origin. Correcting the pre-existing hardcoded repository command host is tracked
separately in #127; this change does not alter it. Live deployment status remains #121.

All 16 walkthrough HTML captures and the account Tokens page passed the customer-copy check:

```sh
grep -Ein 'Zetta|Beast2|Capy|Corosio|admin token' .cache/issue-135/rendered/*.html
```

```text
(no matches; exit 1)
```

## Native proof

The complete static and shared PostgreSQL suites ran against distinct freshly created databases
with separate generated `LAKE_INSTANCE_KEK` values:

```text
$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 229
Total Test time (real) = 284.63 sec

$ ctest --test-dir build-shared --output-on-failure
100% tests passed, 0 tests failed out of 229
Total Test time (real) = 295.95 sec
```

The optional Hugging Face Python-client check skipped and the existing search benchmark remained
disabled in both configurations. Linux proof deferred: the designated `tools/linux-proof.sh`
helper is absent. No Docker commands were run.
