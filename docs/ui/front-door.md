# Customer entry and navigation

Status: the homepage and navigation of the web app (lake#561). The copy is unchanged from the
server-rendered homepage checked on 2026-10-01 against lake `36679e1` on the dev cluster.

The homepage (`/`, `frontend/src/pages/home/HomePage.tsx`) says what Lake is for: designing,
training and shipping AI on the customer's own data, in a workspace of their own. It reads only
`viewer`. Repositories are one link under Ship. Visitors and the operator see it. A signed-in
person lands on Ask Orama instead, full page, and finds the platform at a glance under Home →
Dashboard (`/dashboard`; lake#600, [README.md](README.md#the-shell)).

**Sign-in.** When `LAKE_CLERK_PUBLISHABLE_KEY` is set (`viewer.clerk.enabled`), Sign in opens
Clerk's form and lake's own password, TOTP and passkey pages leave the navigation; see
[`../api/clerk.md`](../api/clerk.md). What a visitor is offered follows the Clerk instance's own
sign-up mode, which the page reads from clerk-js (`userSettings.signUp.mode`, lake#1116), never a
second setting: where it is `restricted` (or `waitlist`), the visitor is offered Sign in only and
the homepage and `/sign-up` say the same sentence about invitations; where it is `public`
(lake.live since 2026-10-24), the visitor is offered **Create an account** as well, with no
invitation sentence. Until clerk-js has answered, the page offers Sign in and the docs only.
Without Clerk, which is what an on-prem install uses, password sign-up is open and the visitor is
offered **Create account** too.

## Landing copy

Every bold phrase in this section is quoted word for word from `frontend/src/pages/home/HomePage.tsx`
(the invitation sentence is `INVITATION_NOTE` in `frontend/src/lib/clerkSignIn.ts`). Change
the page and this section together; the check is under [Verify and change](#verify-and-change).

**Lake by Orama**

**Design, train, and ship AI on your own data.**

**Load your data into your own workspace, train models on it, and call them from your code, in
Orama's cloud or your own.**

Under Clerk with restricted sign-up the visitor actions are **Sign in** and **Read the docs**,
followed by **Accounts are added by invitation while Lake is in development.** Under Clerk with
public sign-up they are **Create an account**, Sign in and Read the docs. Without Clerk they are
**Create account**, Sign in and Read the docs. The operator instead gets **Getting started** (the Docs) and **Load your data**.

Three cards follow, one per verb. Each states only what Lake does today:

1. **Design**. **Start from the business question and the decision it should improve. A project
   turns it into a pipeline you can read, change and approve.** Signed in, it links Projects.
2. **Train**. **Your data lives in your workspace: upload a file, connect a database or generate a
   dataset, then train on it and read what each run found.** Signed in, it links Data and Runs.
3. **Ship**. **Publish a model and call it from your code, publish a search index, or run
   databases for your team. Files and code live in repositories.** Signed in, it links Models and
   Repositories, and an ordinary account also gets **Create a token**.

## Navigation

The rail and top bar come from `NAV` in `frontend/src/routes.tsx` and `components/Shell.tsx`.

| Viewer | Rail | Top bar |
|---|---|---|
| Signed out or dead credential | Home, Public Repositories · **Developer:** Docs | Sign in; also Create account when Clerk is off and password sign-up is open |
| Ordinary account | **Home:** Ask Orama (`/`), Dashboard (`/dashboard`) · **Data:** Repositories, Drives, Shared with Me, Search, Datasets, Transcripts · **Design:** Projects, Opportunities, Solutions, Methodologies, Agents · **Train:** Agents, Runs, Experiments, Streams · **Ship:** Models, Search Indexes, Packages, Apps, Deployments, Services, Workspaces · **Organization:** Organizations, Secrets, Usage, Activity · **Developer:** API, MCP, Docs | Account and workspace picker, Go to, theme, Ask Orama, the account menu: Settings, Sessions, Passkeys (omitted when Clerk is on), Tokens, Sign out |
| Operator | Home, Repositories, Search · **Design** · **Train** · **Ship** (as an account) · **Administration:** Customers, Organizations, Applications, Usage, Secrets, Audit · **Developer:** API, MCP, Docs | the picker when the operator can see an account, Go to, theme, Sign out |

- **Activity** (`/activity`) is the account's own audit events through `my_audit`: newest first,
  principals by name, a workspace call's inner capability, successful reads hidden unless
  **Include reads** is ticked ([audit.md](../api/audit.md#reading-the-log-as-a-person-lake445)).
  **Usage** opens on the account's own account. **API** opens Developer → API (`/developer/api`), whose
  Operations tab renders `/v1/openapi.json` and whose header downloads it.
- **Design, Train and Ship** run on the workspace in the URL, one of the account's workspace Zettas
  ([workspace-selection.md](workspace-selection.md), [`../api/workspaces.md`](../api/workspaces.md)).
- **The current item is chosen by path.** `currentNav` compares the path, without any `/w/<id>`
  prefix, with each item's path and the paths its screen `owns`: an exact match or a parent
  (`/drive` owns `/drive/preview`, and a repository's `/environments` belongs to Repositories). The
  first match wins. The path is never used to authorize.
- **Operators** have no personal pages and get Sign out alone. An operator opening an account page
  is told it belongs to an account.
- **Deployments** (`/deployments`) lists every environment across the repositories the caller may
  list, from `list_repositories` and `list_environments`, and skips a repository whose
  environments the caller may not read.
- Inside a repository the first tab is **Files** (the Drive view) for signed-in viewers, then
  **Code**; the platform tab is **Deployments** (`ForgeTabs` in `pages/repositories/forge.tsx`).
  Anonymous readers get no Files, Deployments or Webhooks tab and no access-details link;
  Protection is offered to operators only, because its page reads the instance audit. Direct
  requests still go through each capability's authorization.

**Customer and operator entry.** A protected page sends a visitor to `/sign-in?next=<page>`.
`/login` is the explicit operator entry (`account/LoginPage.tsx`): an ordinary form posted to
`POST /session`, which compares the token, sets the HttpOnly cookie and redirects. A wrong token
is recorded as a failed sign-in and redirects to `/login?error=That%20token%20did%20not%20match.`,
which the page shows. Its copy points account holders to their own sign-in.

## Verify and change

The landing quotes must match the source. From the repository root:

```sh
python3 - <<'PY'
import re
doc = open("docs/ui/front-door.md").read()
section = doc[doc.index("## Landing copy"):]
section = section[:section.index("\n## ", 1)]
source = re.sub(r"\s+", " ", open("frontend/src/pages/home/HomePage.tsx").read()
                + open("frontend/src/pages/account/signIn.ts").read())
missing = [p for p in (re.sub(r"\s+", " ", q) for q in re.findall(r"\*\*([^*]+)\*\*", section))
           if p not in source]
print("missing:", missing) if missing else print("all quotes present")
PY
```

Then open the app as a visitor (with and without `LAKE_CLERK_PUBLISHABLE_KEY`), as an account and
as the operator, at 1280 and 640 px in both themes, and compare the rail and top bar with the
table above. `HttpServer/HttpResponseIsolation.TheViewerSaysWhoIsSignedInAndWhereTheyAre` checks
the `viewer` read the navigation is built from.

To change the navigation, edit `NAV` in `frontend/src/routes.tsx` (and the screen's `owns` for its
current-item paths) together with this table. New data operations must still be capabilities in the
registry. Do not infer operator status from a page name or a cookie; it is `viewer.administrator`.

## Browser evidence

Historical: this records the 2026-09-08 server-rendered repository homepage, which has since been
replaced twice: by the landing copy above, and by the web app. It is kept as the evidence of that
change; the images show the server-rendered UI.

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

Historical, recorded with the 2026-09-08 change.

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
