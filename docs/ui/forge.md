# Forge pages

Lake's forge is a set of server-rendered views over the repository and forge capabilities. Each
view is a pure C++ renderer: it receives typed values, returns one HTML document, and performs no
I/O. The HTTP adapter obtains every value through `Registry::call`, so the page follows the same
authorization and audit path as REST, OpenAPI, and MCP.

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
identities are also available on hover through `title`.

## Issues

`GET /issues?repository_id=<id>` lists forge issues and accepts state, label, and assignee filters
as an ordinary GET form. Each result keeps its state, labels, assignees, creation date, and compact
identity visible. An unmatched filter combination has a specific empty result rather than an
empty table.

`GET /issue?repository_id=<id>&id=<issue>` renders the issue body and discussion Markdown on the
server, followed by labels, assignees, and linked change identities. Raw HTML in every body is
escaped before Markdown markup is introduced. The page has no local script: links, filters,
discussion, and supporting details follow document order and work unchanged without JavaScript.

## Keyboard and no-script behavior

Source order is skip link, primary navigation, top-bar controls, breadcrumbs, repository tabs,
primary content, then supporting details. All links, native disclosure controls, line anchors, and
theme controls are keyboard focusable. Without JavaScript the repository, tree, and file views
remain readable and navigable. The file-range island only adds shift-click range selection and
range highlighting; individual line anchors remain ordinary links without it. Change review uses
one further island for its view toggle and copy control; unified review remains the default.

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
