# Orama design system

Lake compiles its product-interface tokens from a pinned snapshot of
`oramasearch/orama-design`. The source snapshot is deterministic and offline: only the DTCG JSON
under `vendor/orama-design/tokens/dtcg/` is vendored, and `UPSTREAM.json` records the upstream
commit plus a SHA-256 digest for every file.

## Token pipeline

`design-tokens.map.json` is the reviewed boundary between Lake names and upstream DTCG paths.
`design-tokens.inventory.json` is the stable list of Lake custom properties; the compiler refuses
an unreviewed addition or removal. `tools/ui/build-tokens.mjs` is dependency-free Node and writes
`src/api/web_ui_tokens.inc`, the generated token layer linked into `/style.css`. Do not edit the
generated file or vendored JSON by hand.

To update the snapshot and generated layer:

```sh
node tools/ui/sync-design-system.mjs \
  --from /path/to/orama-design
node tools/ui/build-tokens.mjs
```

To verify the snapshot, generated output, and negative tamper cases:

```sh
./tools/ui/test-token-gates.sh
```

The token layer is light-first. Dark values come from each semantic token's `orama.dark` DTCG
extension. An explicit `data-theme` setting wins over `prefers-color-scheme`; without an explicit
setting, the browser preference selects the theme. Reduced-motion collapses all three upstream
duration tokens.

## Fonts

IBM Plex Sans 400, 500, and 600 and IBM Plex Mono 400 and 500 are self-hosted Latin WOFF2
subsets under `assets/fonts/`. They are sourced from the Fontsource versions pinned by the design
system, and the Sans and Mono OFL-1.1 notices ship beside them. CMake embeds the files into the
binary; Lake serves `/fonts/<face>.woff2` as `font/woff2` with
`public, max-age=31536000, immutable`. The stylesheet declares every face with `font-display:
swap`. No browser request reaches a third-party origin.

## Components and pages

Lake follows the design system's high-density, low-variance product direction: flat surfaces,
hairline boundaries, blue action and focus, green/yellow/red status, violet model and agent
context, and restrained motion. `src/api/web_ui_styles.inc` is the hand-authored component layer;
it consumes only the governed `--lake-*` roles from the generated token include.

Every renderer uses the same application recipe:

1. A skip link enters the focusable main landmark.
2. A sticky sidebar groups workspace, administration, identity, and developer navigation and marks
   the current page with `aria-current="page"`.
3. A sticky top bar carries page context, session controls, and the theme control.
4. A bounded content area presents the eyebrow, one `h1`, supporting copy, actions, panels, and
   dense data regions.

The component contract covers panels and cards; primary, secondary, and destructive buttons;
text, select, checkbox, and textarea controls; compact tables with sticky headers, hover rows,
sort indicators, and expandable identifiers; neutral and semantic badges with status dots; tabs,
empty states, notices, toasts, code blocks, diffs, pagination, breadcrumbs, and inline SVG icons.
Existing page markup uses these shared selectors, so the operator and Lake identity routes move
together without adding renderer-side CSS.
The public transparency monitor uses the same shell and the two shared theme scripts; it adds no
page-specific script. File pages add one local script for line-range selection. The page remains
readable, and each individual line anchor works, when that script does not run. Change pages add
one local script for the diff-layout toggle and full-identity copy control; unified review and
hover disclosure remain available without it.
Forge empty states name the absent record type. The shared pending state is a polite busy status,
and repository errors are alerts with an ordinary recovery link; both use the same shell, focus
order, tokens, and reduced-motion rule as completed pages.

Hosted application identity pages use a compact version of the same token, font, form, focus,
theme, and motion contracts. Their validated application accent is a decorative rule and their
background color is mixed lightly into the governed canvas; readable text and controls retain the
design-system roles in both themes. Application management tables use the shared horizontal
scroll container so their identifiers cannot overlap neighboring forms.

`web_ui_icon(name)` returns decorative, focus-disabled SVG for the approved navigation and theme
names. Accessible names stay on the surrounding link or button. Unknown names return an empty
string so an unreviewed icon cannot silently enter the interface.

## Themes and responsive behavior

The light theme is the default and `prefers-color-scheme` selects dark when there is no explicit
choice. The theme button stores `light` or `dark` in `localStorage["lake-theme"]`; a short script in
the document head applies that preference before CSS loads, avoiding a wrong-theme flash. The
second shared script owns the button and its changing accessible label. No page data is read or
mutated by either script.

Below 58rem, the sidebar becomes a horizontally scrollable navigation region above the page. At
40rem, gutters and headings compact, definition lists stack, and filters fill the available width.
Wide tables remain horizontally scrollable rather than clipping identifiers. Reduced-motion users
get effectively immediate transitions and no smooth scrolling.

## Accessibility contract

- Page structure uses labeled navigation plus `header`, `main`, and supporting `aside` landmarks.
- The skip link, visible focus rings, and source-ordered navigation support keyboard-only use.
- Visible controls have associated labels; icon-only controls carry an accessible label.
- Color is never the only status signal: semantic badges retain text and a dot.
- Action, danger, focus, and status colors keep their governed meanings in both themes.
- Native controls and shared buttons use at least the design system's small control height.

Lake keeps pure C++ renderers; the generated token layer is committed, so neither deployment nor
request handling invokes Node.

### Contrast proof

`node tools/ui/check-contrast.mjs` resolves the committed generated tokens and fails if normal text
or semantic status text is below 4.5:1, or if a focus indicator or strong boundary is below 3:1.
The current output is:

```text
theme  pair                 foreground  background  ratio  min  result
light  primary/canvas       #161616     #f8f8fa     17.06  4.5  PASS
light  secondary/canvas     #525252     #f8f8fa      7.37  4.5  PASS
light  tertiary/canvas      #6f6f6f     #f8f8fa      4.74  4.5  PASS
light  primary/surface      #161616     #ffffff     18.10  4.5  PASS
light  action/canvas        #0f62fe     #f8f8fa      4.72  4.5  PASS
light  on-action/action     #ffffff     #0f62fe      5.00  4.5  PASS
light  success/soft         #198038     #defbe6      4.55  4.5  PASS
light  warning/soft         #8e6a00     #fcf4d6      4.52  4.5  PASS
light  danger/soft          #ba1b23     #fff1f1      5.84  4.5  PASS
light  ai/soft              #6548d8     #f1edff      5.27  4.5  PASS
light  focus/canvas         #0f62fe     #f8f8fa      4.72  3.0  PASS
light  border/surface       #8d8d8d     #ffffff      3.32  3.0  PASS
dark   primary/canvas       #f7f7fa     #0d0d10     18.15  4.5  PASS
dark   secondary/canvas     #b7b8c2     #0d0d10      9.84  4.5  PASS
dark   tertiary/canvas      #9597a3     #0d0d10      6.68  4.5  PASS
dark   primary/surface      #f7f7fa     #151519     17.03  4.5  PASS
dark   action/canvas        #4c82ff     #0d0d10      5.49  4.5  PASS
dark   on-action/action     #ffffff     #0f62fe      5.00  4.5  PASS
dark   success/soft         #46d39a     #123b2d      6.54  4.5  PASS
dark   warning/soft         #f4c84b     #3d3217      7.92  4.5  PASS
dark   danger/soft          #ff6874     #471d25      5.09  4.5  PASS
dark   ai/soft              #9c8cff     #2b234f      5.23  4.5  PASS
dark   focus/canvas         #6b98ff     #0d0d10      6.99  3.0  PASS
dark   border/surface       #606270     #151519      3.02  3.0  PASS
```

## Seeded screenshot fixture

`lake-ui-fixture` runs the production HTTP server with representative customer, organization,
identity, repository, team, SSO, policy, secret, encryption, export, sharing, metering, and audit
records, plus the public transparency monitor. Its on-disk libliquid repository contains history
and a build result.
The live mode of `tools/ui/screenshot.sh` then fetches every page in the generated manifest and
renders both themes at 1440 by 900. Session values and generated identifiers remain under
`.cache`; only the PNG evidence is checked in.
Each capture gives DOM-ready followed by network idle a combined 30-second budget. The Puppeteer
driver records responses and failed requests in `.cache/ui-render/<page>-<theme>-network.json`.
On timeout it writes the partial image, appends the page and network evidence to
`docs/ui/screenshots/after/failures.tsv`, continues the manifest, and fails at the end when that
list is non-empty. This prevents a resource that never settles from blocking the proof run.

The baseline set is in `docs/ui/screenshots/before/<page>-<theme>.png`. The fixture manifest covers
all 60 pages below in light and dark:

```text
home login sign-up sign-in mfa passkey-sign-in settings sessions passkeys tokens
customers organizations organization teams team sso members member invitations grants access
applications application application-users application-organizations application-events
application-sign-up application-sign-in application-mfa application-passkey
repositories repository exports repository-tree file changes change issues issue releases webhooks
protection conflicts
artifacts artifact search secret-mounts secrets encryption-keys metering audit transparency
drive drive-grid drive-preview drive-history drive-version-diff drive-conflict drive-share shared-link
```

The after set uses the same names under `docs/ui/screenshots/after/`, making every comparison a
path-for-path pair. Both directories contain 120 PNG files, and every capture is 1440 by 900.
The routes introduced after a baseline use the closest prior list or detail page in that baseline
slot, so the comparison still records the layout that preceded the new page.
The three primary review screens are the
[repository](screenshots/after/repository-light.png),
[change](screenshots/after/change-light.png), and
[issue](screenshots/after/issue-light.png) pages; each has a neighboring
`-dark.png` capture from the same seeded run.
