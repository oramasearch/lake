# Forge pages

Lake's forge is a set of web-app pages over the repository and forge capabilities
(`frontend/src/pages/repositories/`; shared markup in `forge.tsx`). Each page reads its values
with `GET /v1/<capability>` and changes nothing except through a capability, so it follows the same
authorization and audit path as REST, OpenAPI and MCP.

Anonymous visitors can read issues, reviews, review comments and releases of public repositories.
Private and organization repositories keep their policy checks. Webhook configuration, delivery
history and branch protection evidence need a signed-in reader (their screens' `audience`).

## Repository home

`/repository?id=<id>` (`RepositoryPage.tsx`) presents a repository as a working forge rather than
a metadata record: repository tabs, then the published tree, recent changes and the README on the
left; clone and push commands, references and releases, indexed language and size totals,
effective access and About on the right. The first published reference is the default. An empty
repository keeps the same layout with distinct empty states for files, changes, README, references,
indexed sizes and access. A missing README is expected and shows its empty state; any other README
failure is the page's error.

**Markdown** (README, issues, releases, change messages) is rendered in the browser by
`src/lib/markdown.tsx` with `react-markdown` and `remark-gfm`. Raw HTML in the source stays text,
never markup; an image is named, not loaded ([web app](web-app.md#markdown), lake#943); unsafe link schemes are dropped; every heading gets a stable lowercase anchor
(letters and digits, a run of whitespace or hyphens as one dash), and a repeated heading is numbered
`-2`, `-3`. Links to app pages stay in the app; other links are ordinary links.

**Identifiers.** Long repository, change and reference identities use one compact form (`Identifier`
in `forge.tsx`): 20 characters or fewer stay whole; longer values show the first 12 characters, an
ellipsis and the last 7. The full value is in `title` and `data-full-identifier`; links and calls
use the full value.

**Clone and push.** The commands use `viewer.external_origin`: `LAKE_EXTERNAL_ORIGIN` (or
`--external-origin`), including its scheme and non-default port, else `https://` plus
`LAKE_PUBLIC_HOST`. Request and proxy headers cannot replace either value. With neither configured
the panel explains the missing setting and shows no commands
([../serving/configuration.md](../serving/configuration.md)). The first block runs `liquid clone`
into a new directory and enters it; the second runs `liquid push` from that checkout after changes
have been captured and named. The panel tells the person to create an API token under Account →
**Tokens** with the *liquid and liquify* scopes and export it as `LIQUID_TOKEN` before running
either; the token is never in the URL
([../api/sync.md](../api/sync.md)). URLs and directory names are shell-quoted. A last line names
liquify, liquid's IDE, as the place to open the repository: `liquify '<directory>'` on the checkout,
or a clone from liquify's start screen with the repository URL (lake#1324). The named-lake
form (`liquid lake add <name> <url>` once, then clone and push by name) waits for
[liquid#525](https://github.com/oramasearch/liquid/issues/525); until liquid has that command the
panel keeps the full-URL commands. The liquid releases this Lake supports are in `server_info`
([../api/liquid-clients.md](../api/liquid-clients.md)).
`tests/instance_origin.mjs` executes the two blocks copied from the page and is described in
[../deployment/private-install.md](../deployment/private-install.md).

## Tree and file

`/repository/tree?id=<id>&ref=<ref>` (`TreePage.tsx`) lists every path of the ref recursively, with
kind, mode, logical size and the newest change that touched it; directory rows also match changes
below them. The page derives that from `list_changes` and each change's `change_diff`, a few at a
time. A directory in a file's breadcrumb jumps to its row by anchor.

`/file?repository_id=<id>&ref=<ref>&path=<path>` (`FilePage.tsx`) shows breadcrumbs, the complete
blob metadata, **Raw** and **Download**, stable `L<number>` anchors, selectable `L<first>-L<last>`
ranges (shift-click a line number extends from the last one clicked) and per-line blame. Blame is
resolved newest-first from text-hunk ranges in the change log; a change without text hunks owns the
remaining lines for that version. Raw and Download are lake's `/file/raw` and `/file/download`
routes, which use the same `repository_file` capability and authorization.

C and C++ files are highlighted in the browser (comments, strings and keywords). The syntax-unit
shading comes from lake: `repository_file {units: true}` parses the source with the tree-sitter
grammar target shared by liquid's bundled frontends and returns each unit's line range. Every other
extension uses the same numbered layout with plain text.

## Change review

`/change?repository_id=<id>&id=<change>` (`ChangePage.tsx`) makes the first message line the
review title and renders the rest as Markdown. It shows compact change, parent and blob
identities; the author, each reviewer and each comment's author by name and `@handle`, with the key on
hover ([signers](#signers-by-name)); review state and reviewers (`list_reviews`); per-file and total addition and removal
counts; structural conflict values; line-addressable text hunks with their anchored comments
(`list_review_comments`); and repository checks with output paths and verified attestation profiles
(`list_artifacts`). Change lookup includes changes kept by named review references as well as the
current head, so an open review is readable before it lands. When the repository's workspace
cannot be read, the page says *Reviews are unavailable* with the reason and shows everything else.

Unified is the default layout; **Side by side** switches it, and a button copies the full change
identity. Comment threads follow both layouts and link to their numbered new-line anchor, so
switching never hides discussion. Reviewer, assignee and allowed-pusher identities use the compact
form and wrap as groups.

`/conflicts?repository_id=<id>` (`ConflictsPage.tsx`) presents the current first-class Liquid
conflict records. It names each kind and exact region, explains the conflict in plain words, lists
every suspicious-merge reason, and keeps the base, left, right and optional reviewed proposal
identities as evidence. A signed-in writer resolves a record on `main` with `resolve_conflict`:
keep left, keep right, accept the reviewed proposal, or prepared replacement text.

## What the scenario proves

The Postgres-backed forge scenario starts with a Liquid drip stream and publishes a candidate on a
named review reference while `main` remains at its prior head. A reviewer opens the review through
`liquid::forge_client`, reads the same change, and comments on a source line. The author rebases the candidate as a rewritten change with an inserted line, the
client advances every anchor through Liquid's diff, and the test asserts that the original comment
moves from line 2 to line 3 with its moved disposition preserved. Only the rewritten, approved
change is then published to `main`.

Before landing, the scenario records the graph-selected action and its signed provenance in Lake's
Liquid repository. It asserts that the repository contains no `.yml` or `.yaml` file. The landed
change must carry the graph action's result identity, `build/forge-proof.txt`, and the verified
`slsa_provenance/1` profile. This proves that the visible check describes the recorded
graph result rather than repository automation configuration.

## Issues

`/issues?repository_id=<id>[&state=&label=&assignee=]` (`IssuesPage.tsx`) lists forge issues.
`list_issues` takes no filters, so the page filters itself, and the filters are in the URL:

- **Open, Closed and All** are `StateTabs` with their counts. No `state` is Open (lake#598; before,
  no `state` meant every state), `state=closed` and `state=all` the others.
- **Label** and **Assignee** chips, each with its count; a label chip in a row filters by it too.
- **Search** narrows by title or issue id, in the page only.

Each row shows the title, its compact identity, when it was opened and by whom, its state, its
labels (coloured from their names, `labelTone` in `issues.ts`; lake keeps no label colours), its
assignees by name and when it last changed. The empty states say whether the repository has no
issues, the tab has none, or the filters match none.

**New issue** (the header's primary action, lake#598) opens a drawer (`NewIssue.tsx`): a title, a
Markdown description with **Write** and **Preview** tabs, labels (the repository's labels to toggle
and a new one typed in) and assignees (the repository's people from `issue_assignees`). It
calls `create_issue` and opens the new issue.

`/issue?repository_id=<id>&id=<issue>` (`IssuePage.tsx`) finds the issue in `list_issues` (lake has
no `get_issue`). The header carries the title, the state chip, the identity, who opened it and
when, the comment count, and **Edit**. The main column is the conversation, oldest first: the
description, then each comment (`list_issue_comments`) as Markdown under its author's name and time,
then **Add a Comment** with a preview, **Comment**, and **Close with comment** or **Reopen with
comment**. **Edit** replaces the description with a title field and the Markdown editor. The side
column has **State** (close or reopen), **Labels** and **Assignees** (each changed in place with the
pencil and saved), **Details** and **References** (linked changes and paths).

**Writes and permissions (lake#598).** Every write is the capability REST and MCP call:
`create_issue`, `create_issue_comment`, and `update_issue` sending only the fields the person changed
(`issueChanges`), so two people changing different fields both keep their change. The pages ask
`my_permissions` for `write` on the repository, the action those capabilities check. Without it the
controls stay visible but disabled, with the reason as their tooltip and above the list or the
conversation: signed out, *Sign in with write access…*; signed in, *You can read these issues…*. A
refusal from the capability is shown where the action was taken. Names come from
`issue_assignees`, which needs `write`; a reader sees the compact identity instead, and the
viewer reads as "you". Pure logic is in `issues.ts` and tested in `frontend/tests/issues.test.ts`.

## Releases, artifacts, webhooks and branch protection

`/releases?repository_id=<id>` shows release notes as Markdown and keeps the tag, change identity,
artifacts, output counts and verified attestation counts together, with an explicit empty state
for a release without artifacts.

`/artifacts` pages build results through `list_artifacts`, filtered by action, output and content
(1 to 1000 per page, 100 by default). `/artifact?repository_id=<id>&id=<result>` includes a
**Search inside** form and a member listing. Each output path is searchable as
`in:artifact:<output>@<result>`; text and source members link to their scoped search, binary
members keep their digest without a link. A nested container is opened one level below the output;
deeper nesting is listed but not indexed.

`/webhooks?repository_id=<id>` groups each webhook's sanitized configuration with its durable
delivery attempts: event, attempt number, status, error and outcome. `list_webhooks` removes
secrets. Both lists need write on the repository (lake#1314), so a reader opening the page gets the
page's error state rather than the receiver URLs. A failed attempt offers **Retry** (`retry_webhook_delivery`), which creates a new attempt
and leaves every earlier one visible.

`/protection?repository_id=<id>` shows each ref pattern with its review, verification,
fast-forward, allowed-pusher and policy-object values, and the denied
`forge.branch_protection.refused` events for that repository from `audit` (its workspace's log,
or the central log when the repository has no workspace): reference, caller, matching rule,
reason and date. Rules and refusals each have an empty state.

## Empty, loading and error states

Every forge collection and supporting section names its empty state in place: repositories, trees,
recent changes, README content, refs, access, diffs, reviews, comments, checks, issues, releases,
artifacts, deliveries, rules and refused updates never collapse into an unexplained blank region.
A failed read is an error, never an empty list.

While a page's required reads load it shows the kit's skeleton. A failed read shows lake's error
page: the error code, *We Could Not Load This Page*, the message, and **Back to repositories**. A
signed-out visitor refused a repository is sent to `/sign-in?next=…` instead, so a private
repository's existence is not disclosed (`RepositoryError` in `forge.tsx`).

## Keyboard

Source order is skip link, rail, top bar, breadcrumb, repository tabs, primary content, then
supporting details. All links, disclosure controls, line anchors and buttons are keyboard
focusable. Issue filters come before results, discussion before supporting metadata, and a state
page's recovery action after its message.

## What changed (lake#561)

The pages render in the browser instead of from C++ renderers, so the no-script behaviour the
server pages guaranteed (the tree as `details`, the file range and diff toggle as optional
scripts) no longer applies: the app needs JavaScript. Markdown and C/C++ highlighting moved to the
browser with the same rules; syntax units still come from lake.

## Layout (lake#561 redesign)

The repository, forge and Drive pages follow the app's rev-os look (`web-app.md` → *The look*) and
share one frame, `ForgePage` in `forge.tsx`:

- **Header.** The eyebrow is the rail group, **Data**; the `<h1>` names the page or entity; under it
  the repository it belongs to (`RepositoryLine`: its name as the way back and its visibility chip);
  one sentence; the actions on the right. Then the repository tabs (`tab`), and above the header,
  on tree, file and Drive pages, the path bar (`PathBreadcrumbs`, `DriveBreadcrumbs`, styled as a
  location bar in `styles/repositories.css`).
- **Figures.** A `StatRow` from the reads the page already makes, never a new read: files, changes,
  references and releases on the repository home; files changed, lines and checks on a change;
  open, closed, labels and unassigned on Issues; and so on. No figure is invented.
- **Lists.** A toolbar over every list (a search that filters in the page, filter chips for the
  states and kinds the rows carry) and rich rows: the name as a link with a muted second line,
  states as state chips (`ForgeStatus`: lake's forge colour, else the word's usual colour), tags
  as chips, times relative with the full time on hover.
- **Detail pages are split.** Main content on the left; a side column of cards with the facts,
  state and secondary actions. The repository home puts the files, recent changes and README on the
  left and Clone and Push (copy blocks), About, references, releases, languages (meters) and
  effective access on the right; Clone also opens the same commands in a dialog. A change review
  puts its description and file list beside its details, reviews and checks, then the diffs in
  full width with lake's diff classes.
- **Creation and edits in dialogs.** New repository is a drawer from the list header; Drive's New
  folder, Rename and Move, and Sharing's People, Organization and Link deliveries are dialogs;
  deleting a Drive item, revoking a link from its page and restoring a version are confirmations
  that say what happens.
- **Empty states** have an icon, a title, one sentence and the next action; a failed read still
  shows lake's error page, never an empty list.

The one pattern the kit does not draw is the item menu (`Menu` in `forge.tsx`): a "⋯" button
whose menu is drawn in a portal at the button, so a scrolling table does not clip it. Drive uses
it for each item and for the folder's History and Conflicts.

## Verify

`npm run build` in `frontend/` type-checks the pages. With a schema-loaded `LAKE_TEST_DB_URI`,
`lake-tests --gtest_filter='PostgresForgeScenario.*'` runs the complete review lifecycle. In a browser, open a public repository signed out and a private one signed in,
and check the README's anchors and an embedded `<script>` shown as text, a C++ file's highlighting
and `#L5-L9`, a change in both layouts with comments, issue filters, a webhook retry, and the
protection page as the operator.

## Change

Add the data to a capability's output first ([../api/web-app.md](../api/web-app.md#change)), then
read it in the page. Shared presentation belongs in `styles/lake.css` and the kit; pages use lake's
classes, not their own colours.

## Signers by name

The repository page's Recent changes and Effective access, the Change Log, a change's header,
details, reviewers and comments, the File page's blame and the repository's Effective Access page
show who signed a change or holds access by name and `@handle` (`SignerName` in `forge.tsx`), from
the `people` member of the read that named them ([../api/repositories.md](../api/repositories.md#signers-by-name),
lake#1326). The whole key is in the `title` (the hover) and `data-full-identifier`. An identity no
account acts for reads **Unknown signer** and a short key (the key without `taap:1:ed25519:`, first 8
and last 4 characters). A principal that is not a TAAP identity (an App, a build, a share link) reads
as itself. The viewer's own identity adds "(you)". The Change Log's search and the access search
match names and handles as well as keys. The labels are pure functions in `signers.ts`
(`frontend/tests/signers.test.ts`).

