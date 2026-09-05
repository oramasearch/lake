# Forge pages

Lake's forge is a set of server-rendered views over the repository and forge capabilities. Each
view is a pure C++ renderer: it receives typed values, returns one HTML document, and performs no
I/O. The HTTP adapter obtains every value through `Registry::call`, so the page follows the same
authorization and audit path as REST, OpenAPI, and MCP.

Anonymous visitors can read issues, reviews, review comments, and releases for public
repositories. Private and organization repositories retain their policy checks. Webhook
configuration, delivery history, and branch protection evidence still require an authenticated
reader.

## Repository home

`GET /repository?id=<id>` presents a repository as a working forge rather than a metadata record.
The page leads with repository navigation, then shows the published tree, recent changes, a
server-rendered README, references and releases, clone and push commands, indexed language and
logical-size totals, and effective access. The first published reference is the default. An empty
repository keeps the same layout and gives distinct empty states for files, changes, README,
references, indexed sizes, and access.

README Markdown is parsed on the server. Raw input is escaped before the renderer emits any
markup, inline code stays escaped, and headings receive stable lowercase anchors. Repeated headings
receive numbered anchors. Raw content HTML is always text; it never enters the response as markup.

Long repository, change, and reference identities use the shared compact form: values of 20 or
fewer characters remain complete; longer values show the first 12 characters, an ellipsis, and the
last 7 characters. The full value remains in the element's `title` and `data-full-identifier`
attributes. Pages use the complete value for links and capability calls.

The tree uses native `details` and `summary` elements for expansion, so the structure remains
available without JavaScript. Clone and push examples use the repository sync URL documented in
[`../api/sync.md`](../api/sync.md) and keep credentials in `LIQUID_TOKEN`, never in the URL.

## Tree and file

`GET /repository/tree?id=<id>&ref=<ref>` presents recursive entries as a file browser. Each row
keeps the entry kind, mode, logical size, and newest change that touched the path. Directory rows
also match changes below their path. The HTTP adapter derives this annotation from `list_changes`
and `change_diff`; the renderer receives only typed values.

`GET /file?repository_id=<id>&ref=<ref>&path=<path>` renders breadcrumbs, complete blob metadata,
raw and download links, stable `L<number>` anchors, selectable `L<first>-L<last>` ranges, and
per-line blame. Blame is resolved newest-first from text-hunk ranges in the change log; a change
without text hunks owns the remaining lines for that version. `GET /file/raw` returns source as
plain text and `GET /file/download` adds an attachment disposition. Both use the same
`repository_file` capability and authorization as the HTML page.

C and C++ extensions are parsed server-side with the tree-sitter grammar target shared by
liquid's bundled frontends. Parsed source units and a small token renderer add syntax roles. The
recognized extension list matches those frontends. Every other extension uses the same numbered
source layout with fully escaped plain text.

## Change review

`GET /change?repository_id=<id>&id=<change>` makes the first message line the review title and
renders the remaining Markdown on the server with the same raw-HTML prohibition as README files.
It shows compact change, author, parent, and blob identities; review state and reviewers; per-file
and aggregate addition/removal counts; structural conflict values; line-addressable text hunks and
their anchored comments; and repository checks with attestation counts. Reviews and comments are
selected by change and review identity after their capability reads.

The unified diff is the no-script view. One page-local vanilla script switches to the side-by-side
representation and copies the complete change identity. If the script is unavailable, every file,
hunk, comment, conflict, check, attestation count, and full identity remains present. Full
identities are also available on hover through `title`. Comment threads follow both diff
representations and link back to their numbered new-line anchor, so switching layouts never hides
discussion. Reviewer, assignee, and allowed-pusher identities use the same compact rule and wrap
as groups rather than overflowing their panels.

## Issues

`GET /issues?repository_id=<id>` lists forge issues and accepts state, label, and assignee filters
as an ordinary GET form. Each result keeps its state, labels, assignees, creation date, and compact
identity visible. An unmatched filter combination has a specific empty result rather than an
empty table.

`GET /issue?repository_id=<id>&id=<issue>` renders the issue body and discussion Markdown on the
server, followed by labels, assignees, and linked change identities. Raw HTML in every body is
escaped before Markdown markup is introduced. The page has no local script: links, filters,
discussion, and supporting details follow document order and work unchanged without JavaScript.

## Releases, webhooks, and branch protection

`GET /releases?repository_id=<id>` renders release notes through the safe Markdown path and keeps
the tag, change identity, artifacts, output counts, and verified attestation counts together. It
shows an explicit release-level artifact empty state when older records have none.

`GET /webhooks?repository_id=<id>` groups each sanitized webhook configuration with its durable
delivery attempts: event, attempt number, status, error, and delivery outcome. Secrets are removed
by `list_webhooks` and are never renderer input. Failed attempts expose an ordinary POST retry form
to `/webhooks/retry`; successful deliveries and signed-out views do not. The POST calls
`retry_webhook_delivery`, whose redirect returns to the history page and leaves every attempt
visible.

`GET /protection?repository_id=<id>` shows each ref pattern with its review, verification,
fast-forward, allowed-pusher, and policy-object values. It also reads the signed audit capability
and selects denied `forge.branch_protection.refused` events for that repository, retaining the
reference, caller, matching rule, reason, and event date. Rules and the refusal audit each have a
separate empty state. All three pages are server-rendered and add no local script.

## Empty, loading, and error states

Every forge collection and supporting section names its empty state in place: repositories,
trees, recent changes, README content, refs, access, diffs, reviews, comments, checks, issues,
releases, artifacts, deliveries, rules, and refused updates never collapse into an unexplained
blank region.

The shared loading renderer uses `role="status"`, polite announcements, and `aria-busy="true"`.
Its animation uses governed motion tokens and is reduced by the global reduced-motion rule. The
HTTP adapter waits for capability reads before sending forge pages, so this state is intended for
hosts that choose to display a pending server navigation; no client data loader is introduced.

Repository capability errors render through the shared forge error page with the real HTTP status,
an alert role, escaped error detail, and a keyboard-accessible route back to repositories.
Anonymous authorization failures continue to redirect to sign-in without disclosing repository
existence.

## Keyboard and no-script behavior

Source order is skip link, primary navigation, top-bar controls, breadcrumbs, repository tabs,
primary content, then supporting details. All links, native disclosure controls, line anchors, and
theme controls are keyboard focusable. Without JavaScript the repository, tree, and file views
remain readable and navigable. The file-range island only adds shift-click range selection and
range highlighting; individual line anchors remain ordinary links without it. Change review uses
one further island for its view toggle and copy control; unified review remains the default.
Issue filters precede results, issue discussion precedes supporting metadata, and forge state pages
place their primary recovery action after the state message.

## Verify

Run `lake-tests --gtest_filter='WebUi.Markdown*:WebUi.Repositories*:WebUi.RepositoryContent*'`.
The string tests prove raw HTML is escaped, heading anchors are stable, repository sections are
present, tree rows carry last-change context, known source is highlighted, line anchors and blame
are rendered, review Markdown is sanitized, review state and comments are anchored, checks and
attestations are present, long identities are compact, and the shared shell remains intact. The
seeded visual proof is generated through `tools/ui/screenshot.sh` and recorded in
`docs/ui/screenshots/`.

## Change

Add page data to the typed values in `include/lake/api/web_ui.hpp`, fetch it through capabilities
in `src/api/http_server.cpp`, and render it in `src/api/web_ui.cpp`. Shared presentation belongs in
`src/api/web_ui_styles.inc`; page-specific inline styles are not allowed. A new interaction must
remain useful without JavaScript and may add one small vanilla script only when the interaction is
named in bridge decision 17.
