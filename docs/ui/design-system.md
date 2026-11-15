# Orama design system

Lake compiles its product-interface tokens from a pinned snapshot of
`oramasearch/orama-design`. The source snapshot is deterministic and offline: only the DTCG JSON
under `vendor/orama-design/tokens/dtcg/` is vendored, and `UPSTREAM.json` records the upstream
commit plus a SHA-256 digest for every file.

## Token pipeline

`design-tokens.map.json` is the reviewed boundary between Lake names and upstream DTCG paths.
`design-tokens.inventory.json` is the stable list of Lake custom properties; the compiler refuses
an unreviewed addition or removal. `tools/ui/build-tokens.mjs` is dependency-free Node and writes
the same token layer twice:

- `frontend/src/styles/tokens.css`, which the web app imports;
- `src/api/web_ui_tokens.inc`, the server's copy, still used by the pages lake renders itself
  (an identity application's hosted sign-in pages).

Do not edit either generated file or the vendored JSON by hand.

To update the snapshot and generated layers:

```sh
node tools/ui/sync-design-system.mjs \
  --from /path/to/orama-design
node tools/ui/build-tokens.mjs
```

To verify the snapshot, both generated outputs, and negative tamper cases:

```sh
node tools/ui/build-tokens.mjs --check
./tools/ui/test-token-gates.sh
```

The token layer is light-first. Dark values come from each semantic token's `orama.dark` DTCG
extension. An explicit `data-theme` setting wins over `prefers-color-scheme`; without an explicit
setting, the browser preference selects the theme. Reduced-motion collapses all three upstream
duration tokens.

## How the app uses it

`frontend/src/index.css` is the whole stylesheet, in four cascade layers:

| Layer | Source | What |
|---|---|---|
| `theme` | `tailwindcss/theme.css` | Tailwind's theme variables, then lake's binding (below) |
| `base` | `styles/tokens.css`, `styles/fonts.css` | the generated tokens and the IBM Plex faces |
| `components` | `styles/lake.css`, `styles/app.css` | lake's component classes, moved from `src/api/web_ui_styles.inc`; what the app adds (overlays, the collapsible rail and drawer, the command palette, fields, the account menu and Ask Orama panel) |
| `utilities` | `tailwindcss/utilities.css` | layout utilities |

Two pages import their own small sheet, both in lake tokens only: `styles/account.css` (the bare
sign-in frame) and `styles/experiments.css` (the chart pages, scoped to `.experiments`).

- **Tailwind without preflight.** Only Tailwind's theme and utilities are imported, never its
  base reset, so lake's own base styles in `lake.css` stay authoritative.
- **Tailwind is bound to the tokens.** `@theme inline` clears Tailwind's colours
  (`--color-*: initial`) and maps every colour, both font families, the three radii and the two
  shadows to `--lake-*` roles (`bg-surface`, `text-ink-2`, `border-hairline`, `rounded-panel`, …).
  A utility cannot bring in a colour outside the design system.
- **Dark mode** is `data-theme="dark"` on `<html>` (`@custom-variant dark`).
- **Lake's class names are the kit's vocabulary.** The kit in `src/lib/ui.tsx` renders lake's
  markup (`panel`, `card`, `badge`, `button`, `notice`, `empty`, …), so a page reads like the
  server pages did. Overlays (`src/lib/overlay.tsx`: modal, drawer, confirmation) are the only
  surfaces with a shadow.

## Fonts

IBM Plex Sans 400, 500, and 600 and IBM Plex Mono 400 and 500 are self-hosted Latin WOFF2
subsets under `assets/fonts/`. They are sourced from the Fontsource versions pinned by the design
system, and the Sans and Mono OFL-1.1 notices ship beside them. `styles/fonts.css` declares every
face with `font-display: swap` and points at those files; Vite copies them into `/ui/` under
content-hashed names (`ibm-plex-sans-400-<hash>.woff2`). No browser request reaches a third-party
origin. The server still serves the same files at `/fonts/<face>-<hash>.woff2` for its own pages.

## Asset URLs and caching

Every file the app loads after `index.html` is a content-hashed name under `/ui/`, served
`Cache-Control: public, max-age=31536000, immutable`; `index.html` is `no-cache`, so a new build is
picked up on the next page load while unchanged chunks stay cached. A byte change produces a new
name. The files are embedded in the binary; nothing is read from disk at run time
([web-app.md](web-app.md)).

## Components and pages

Lake follows the design system's high-density, low-variance product direction: flat surfaces,
hairline boundaries, blue action and focus, green/yellow/red status, violet model and agent
context, and restrained motion.

Every page in the shell uses the same frame (`components/Shell.tsx`):

1. A skip link enters the focusable main landmark.
2. A rail shows the viewer's destinations and marks the current page with `aria-current="page"`.
   [Front-door navigation](front-door.md#navigation) records the visitor, account, and operator
   matrix.
3. A sticky top bar carries the picker, the breadcrumb, Go to, the theme switch and the session
   controls. The root's `scroll-padding-top` (top-bar height plus `--lake-space-4`) lands a
   `#fragment` target below the bar instead of under it (lake#438).
4. A bounded content area presents the eyebrow, one `h1`, supporting copy, actions, panels, and
   dense data regions (`Page` in `src/lib/ui.tsx`).

The component contract covers panels and cards; primary, secondary, and destructive buttons;
text, select, checkbox, and textarea controls; compact tables with sticky headers and hover rows;
neutral and semantic badges with status dots; tabs, empty states, notices, toasts, code blocks,
diffs, pagination, breadcrumbs, and icons. Empty states name the absent record type, and a failed
read is shown as an error, never as an empty list (`npm run build` refuses a list without its
error state, `scripts/check-list-errors.mjs`).

Icons: `components/Icon.tsx` draws lake's own 16 × 16 navigation and theme icons and returns
nothing for an unknown name, so an unreviewed icon cannot slip in. The app also uses
`lucide-react` for a few controls (the Ask Orama panel's buttons). Icons are decorative
(`aria-hidden`); the accessible name stays on the surrounding link or button. The one exception is
the catalogue icons of solutions and methodologies (`components/CatalogueIcon.tsx`, lake#639): they
are hand-drawn two-tone pictures on lake's tokens, each an image with a label saying what it shows.
Their grid, stroke, palette and motifs are in [catalogue-icons.md](catalogue-icons.md).

Hosted application identity pages are still rendered by lake. They use a compact version of the
same token, font, form, focus, theme, and motion contracts. Their validated application accent is a
decorative rule and their background colour is mixed lightly into the governed canvas; readable
text and controls retain the design-system roles in both themes.

## Capitalization

Nav items and headings are **Title Case** ("Search Indexes", not "Search
indexes").

| Where | Case | Examples |
|---|---|---|
| Rail links and group labels (`NAV` in `frontend/src/routes.tsx`) | Title Case | Projects, Search Indexes, Shared with Me, Public Repositories |
| Screen titles (`routes.ts`), page and section titles, `<h1>`–`<h4>` | Title Case | API Tokens, Connect One of Your Services, Sign In with a Passkey, Who Can Access This Folder Now |
| Body text, buttons, form labels, table headers, eyebrows | Sentence case | *Start your workspace* (button), *The decision it should improve* (label) |
| A heading that is a full sentence ending in `.`, `?` or `!` | Sentence | *Design, train, and ship AI on your own data.* |

Title Case here: capitalize every word except articles (*a, an, the*), coordinating conjunctions
(*and, but, or, nor, for, so, yet*) and prepositions of four letters or fewer (*as, at, by, in, of,
on, to, up, via, per, with, from, into, onto, over, off*); those stay lowercase unless they are the
first or last word. Acronyms keep their letters ("API", "SSO", "TAAP"); hyphenated words capitalize
each part ("Multi-Factor"); a verb's particle is capitalized ("Sign In", "Set Up"). Names a person
typed (a project, a repository, a file path) are shown exactly as typed, never re-cased.

Tests that look for a heading search for its Title Case text.

## Themes and responsive behavior

The light theme is the default and `prefers-color-scheme` selects dark when there is no explicit
choice. The theme button stores `light` or `dark` in `localStorage["lake-theme"]`; a short script
in `index.html` applies it before the styles load, avoiding a wrong-theme flash. Charts redraw
when the theme changes.

At 1024 px and wider the rail is a sticky column that can collapse to icons. Below 1024 px it is
an off-canvas drawer, opened from the menu button and closed by Escape, the scrim or navigation.
At 40rem, gutters and headings compact, definition lists stack, filters fill the width, the
breadcrumb shows its last two levels, and the picker takes its own row
([workspace-selection.md](workspace-selection.md#the-picker)). Wide tables scroll horizontally
rather than clipping identifiers. Reduced-motion users get effectively immediate transitions and
no smooth scrolling.

## Accessibility contract

- Page structure uses labeled navigation plus `header`, `main`, and supporting `aside` landmarks.
- The skip link, visible focus rings, and source-ordered navigation support keyboard-only use;
  Go to opens with ⌘K or `/`, and **G** then a letter jumps to a rail group.
- Visible controls have associated labels; icon-only controls carry an accessible label.
- Color is never the only status signal: semantic badges retain text and a dot.
- Action, danger, focus, and status colors keep their governed meanings in both themes.
- Native controls and shared buttons use at least the design system's small control height.
- Overlays move focus inside when they open and return it to the opener when they close; Escape
  closes them, and an Escape or Cancel on a confirmation sends no action.

## Contrast proof

`node tools/ui/check-contrast.mjs` (CTest `lake-ui-contrast`) resolves the committed generated tokens (`src/api/web_ui_tokens.inc`, identical to `tokens.css`) and fails if normal text
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

**Historical: these images show the server-rendered UI**, before lake#561. They are kept for
comparison with the web app and are not regenerated; the folder list is in
[README.md](README.md#screenshots).

`lake-ui-fixture` runs the production HTTP server with representative customer, organization,
identity, repository, team, SSO, policy, secret, encryption, export, sharing, metering, and audit
records, plus the public transparency monitor. Its on-disk libliquid repository contains history
and a build result.
The live mode of `tools/ui/screenshot.sh` then fetches every page in the generated manifest and
renders both themes at 1440 by 900. Session values and generated identifiers remain under
`.cache`; only the PNG evidence is checked in.
Each capture gives DOM-ready followed by network idle a combined 30-second budget. The Puppeteer
driver records responses and failed requests in `.cache/ui-render/<page>-<theme>-network.json` using
worktree-relative paths inside its container.
On timeout it writes the partial image, appends the page and network evidence to
`docs/ui/screenshots/after/failures.tsv`, continues the manifest, and fails at the end when that
list is non-empty. This prevents a resource that never settles from blocking the proof run.

The baseline set is in `docs/ui/screenshots/before/<page>-<theme>.png`. The fixture manifest covers
all 62 pages below in light and dark:

```text
home login sign-up sign-in mfa passkey-sign-in settings sessions passkeys tokens
customers organizations organization teams team sso members member invitations grants access
applications application application-users application-organizations application-events
application-sign-up application-sign-in application-mfa application-passkey
repositories repository exports repository-tree file changes change issues issue releases webhooks
protection conflicts
artifacts artifact search secret-mounts secrets encryption-keys metering audit transparency
drive drive-grid drive-preview drive-history drive-version-diff drive-conflict drive-share
drive-share-link shared-with-me shared-link
```

The fixture is in the test suite (lake#497). `lake-ui-fixture-pages` runs
`tools/ui/test-fixture-pages.sh`, which starts the fixture on a free port and fails when the
fixture does not start, when a manifest page does not answer 200 with HTML, or when any GET route
the server registered answers 500 or above, or no answer at all, with the fixture user's session,
the operator token, or no session. The fixture writes those routes beside the manifest as
`<manifest>.routes`; route parameters are the `research` workspace or the literal `fixture`.
Password sign-up provisions each user's personal customer, so the fixture reads that customer
instead of creating one.

The after set uses the same names under `docs/ui/screenshots/after/` and contains 124 PNG files;
the four newest sharing captures use the closest prior list or detail page in the baseline slot.
Every capture is 1440 by 900.
The three primary review screens are the
[repository](screenshots/after/repository-light.png),
[change](screenshots/after/change-light.png), and
[issue](screenshots/after/issue-light.png) pages; each has a neighboring
`-dark.png` capture from the same seeded run.
