# Web UI visual direction mocks

These static mocks put the same dense repository, change review, and organization data into three
visual directions before Lake adopts a design system. They are reference material only: they do
not change `src/api/web_ui.cpp`, `/style.css`, or any served page.

## Open the mocks

Open any HTML file directly in a browser; no server, build, JavaScript, framework, or network
resource is required. On macOS, for example:

```sh
open docs/design/ui/mocks/quiet/repository.html
open docs/design/ui/mocks/orama/change.html
open docs/design/ui/mocks/editorial/organization.html
```

Each folder contains `repository.html`, `change.html`, and `organization.html`, with direction
tokens in `tokens.css` and reusable UI rules in `components.css`. Quiet and editorial include light
and dark token sets; change the root `data-theme="light"` attribute to `data-theme="dark"` to view
the dark variant. Orama is light-only because the captured public brand page presents a light
application language and does not establish a dark palette.

The content deliberately includes nested paths, full UUIDs and digests, a README, references,
effective access, two teams, a pending invitation, a side-by-side C++ and documentation diff, two
per-line comments, and a recorded conflict value. Identifiers truncate only where the component is
intended to stay one line; diff and metadata regions otherwise expose the pressure created by real
data.

## Quiet

Quiet is a calm developer-tool register: neutral surfaces, one blue accent, system type, compact
panels, and a persistent sidebar. It aims to let hierarchy and data state carry more weight than
brand expression in both light and dark environments.

The risk is anonymity: on dense tables and diffs, the restrained hierarchy can make adjacent data
regions feel interchangeable, while small labels become tiring if overused. Across thirty pages,
consistency should be straightforward because the palette and components are narrow, but pages
with a different job may become visually indistinct unless information architecture stays strong.

## Orama

Orama translates the current public family into an application register: near-black heavy
headings, cool white surfaces, technical grid lines, monospaced micro-labels, and a restrained red
accent. It aims to make Lake feel immediately related to Orama without turning a data tool into a
marketing page.

The risk is density conflict: the public-site scale and breathing room cannot survive unchanged in
long tables or diffs, and the red accent can read as an error if it carries too many roles. Across
thirty pages, the grid, outlined cards, strong headings, and label treatment need explicit usage
rules or individual pages will drift between marketing scale and application density.

The source was [Orama's public site](https://orama.com/) captured on 2026-09-05; it exposed
`#BF3536` as the primary/accent color, `#0C1014` text, cool grey borders and surfaces, heavy Geist
headings, system-oriented body type, compact rounded controls, and technical diagrams. These mocks
embed no Orama logo, image, or font file: the radial mark is newly drawn in CSS for this comparison,
and system font stacks approximate the observed type roles, so no third-party asset licence is
required. Capture evidence stays in the ignored `.firecrawl/` working directory and is not part of
the repository; the Orama name and trademarks remain the property of their owner.

## Editorial

Editorial gives Lake a more distinctive publication-like identity: serif display type, humanist
UI text, warm paper surfaces, square rules, generous section spacing, and sparse brick-red state.
It aims to make changes and repository narratives feel authored while retaining monospace for raw
data in both light and dark variants.

The risk is vertical cost: generous rhythm competes directly with long trees, review threads, and
wide metadata, while serif labels can slow scanning when every row carries them. Across thirty
pages, the contrast between narrative and utility type needs disciplined templates; without them,
each page could become a bespoke composition and dense administrative screens would lose parity.

## Validate and render

HTML is validated locally with `html-validate` 10.4.0. Keep its npm cache inside the repository's
ignored `.cache/` directory:

```sh
npm_config_cache="$PWD/.cache/npm" npx --yes html-validate@10.4.0 \
  "docs/design/ui/mocks/{quiet,orama,editorial}/*.html"
```

Render the screenshot set with:

```sh
./tools/ui/screenshot.sh
```

The renderer uses `chromium`, `google-chrome`, `google-chrome-stable`, or `chromium-browser` when
one is installed. Otherwise it runs the pinned
`ghcr.io/puppeteer/puppeteer:24.16.0` Docker image and its bundled Chromium; Docker must be running
and may pull the image on the first invocation. It writes 1440×900 PNGs to `screenshots/` and uses
temporary copies of the HTML to switch quiet and editorial to their dark tokens.

The complete set is:

- `screenshots/quiet-repository-light.png`
- `screenshots/quiet-repository-dark.png`
- `screenshots/quiet-change-light.png`
- `screenshots/quiet-change-dark.png`
- `screenshots/quiet-organization-light.png`
- `screenshots/quiet-organization-dark.png`
- `screenshots/orama-repository-light.png`
- `screenshots/orama-change-light.png`
- `screenshots/orama-organization-light.png`
- `screenshots/editorial-repository-light.png`
- `screenshots/editorial-repository-dark.png`
- `screenshots/editorial-change-light.png`
- `screenshots/editorial-change-dark.png`
- `screenshots/editorial-organization-light.png`
- `screenshots/editorial-organization-dark.png`

## Change a direction

Change colors, type scale, spacing, radii, and elevation in that direction's `tokens.css`; change
layout and components in `components.css`; keep the three HTML documents structurally equivalent
so screenshots remain comparable. Re-run HTML validation and regenerate every screenshot after a
markup or style change, then inspect all themes at their committed 1440×900 size.
