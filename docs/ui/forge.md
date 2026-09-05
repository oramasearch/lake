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

## Keyboard and no-script behavior

Source order is skip link, primary navigation, top-bar controls, repository tabs, primary content,
then supporting details. All links, native disclosure controls, and theme controls are keyboard
focusable. Without JavaScript the full repository home remains readable and navigable; only the
shared theme button stops changing themes.

## Verify

Run `lake-tests --gtest_filter='WebUi.Markdown*:WebUi.Repositories*'`. The string tests prove raw
HTML is escaped, heading anchors are stable, repository sections are present, long identities are
compact, and the shared shell remains intact. The seeded visual proof is generated through
`tools/ui/screenshot.sh` and recorded in `docs/ui/screenshots/`.

## Change

Add page data to the typed values in `include/lake/api/web_ui.hpp`, fetch it through capabilities
in `src/api/http_server.cpp`, and render it in `src/api/web_ui.cpp`. Shared presentation belongs in
`src/api/web_ui_styles.inc`; page-specific inline styles are not allowed. A new interaction must
remain useful without JavaScript and may add one small vanilla script only when the interaction is
named in bridge decision 17.
