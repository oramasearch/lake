# Customer entry and navigation

The homepage explains hosted Liquid repositories and the account → repository → push path that
personal customer provisioning already supports. It uses the existing design-system shell,
panels, grid, and theme controls. No schema or capability was added.

## Landing copy

**Repositories for your work**

**Keep your code and files together.**

Lake hosts your Liquid repositories so you can store your work, browse its history, and share it
with other people.

The visitor actions are **Create account** and **Sign in**. An authenticated visitor instead gets
**Open your repositories**.

**From your first account to your first push**

1. **Create an account.** Use your email, a password, and your public Liquid identity document.
   You get a personal space with a generated handle that you own. Sign-up explains how to install
   Liquid and export the public document. Installing Liquid currently requires access to its
   source repository.
2. **Create a repository.** Choose a name in your personal space. Your new repository starts
   private, and you can manage it with your account.
3. **Push with Liquid.** Liquid is the command-line tool that sends your local work to Lake.
   Create a token with write access in your account's Tokens page. Use it as `LIQUID_TOKEN` when
   you run `liquid push` with your repository's URL.

The Tokens page gives the existing sync capability scopes:
`refs,find_missing,push,pull,upload_chunks,read_chunk`. Tokens retain the user's repository policy;
the homepage and forms do not confer additional access.

## Navigation

| Viewer | Workspace | Account | Administration | Developer | Session |
|---|---|---|---|---|---|
| Signed out or invalid credential | Home, Public repositories | — | — | — | Sign in, Create account |
| Ordinary account | Home, Repositories, Shared with me, Search | Settings, Sessions, Passkeys, Tokens | — | API | Sign out |
| Operator | Home, Repositories, Search | — | Customers, Organizations, Applications, Metering, Secrets, Audit | API | Sign out |

Previously, every viewer saw Home, Repositories, Shared with me, Search, Customers, Organizations,
Applications, Metering, Secrets, Audit, and API, plus an implementation label in the sidebar footer.
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
own sign-in. OpenAPI now describes account tokens and public operations accurately.

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

For browser verification, start the native server on a separate fresh database. Open a new profile
at each of 1280 and 640 pixels in light and dark, follow the homepage's Create account action,
export a fresh public Liquid identity document, submit the form, and create the first private
repository. Save the rendered customer pages and check their text for implementation names and
operator-credential instructions. The recorded browser evidence follows.

## Browser evidence

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
