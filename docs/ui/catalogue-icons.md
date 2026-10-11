# Catalogue Icons

Every solution and methodology in Orama's catalogue has its own icon (lake#639): 20 solutions
(S-1 to S-12, B-1 to B-8) and 21 methodologies (M-1 to M-21), as the engine lists them. The icons
show on Design → Solutions and Methodologies, on each solution's and methodology's page, on the
Opportunities cards, beside a solution's methodologies, and as hits of Search's **Solutions** place.
**Design → Catalogue Icons** (`/design/icons`, not in the rail, open to everyone) shows the whole set
at 20, 32 and 64 px with the fallbacks, for review.

This page is the style sheet. Draw a new icon to it so the set stays one family.

## Where They Live

| What | Where |
|---|---|
| The glyphs, their labels and motifs, the fallback rule (source of record) | `frontend/src/lib/catalogueIcons.ts` |
| Drawing one in the app | `frontend/src/components/CatalogueIcon.tsx` (`SolutionIcon`, `MethodologyIcon`) |
| Colours | `frontend/src/styles/catalogue-icons.css` |
| The gallery | `frontend/src/pages/design/IconsPage.tsx` |
| The website's copy, one standalone SVG per icon | `assets/catalogue-icons/{solutions,methodologies,families}/*.svg`, written by `node tools/ui/export-catalogue-icons.mjs` |
| The test | `frontend/tests/catalogue-icons.test.ts` (`npm test`) |
| The browser check | `frontend/checks/catalogue-icons.mjs`; screenshots in [screenshots/catalogue-icons/](screenshots/catalogue-icons/) |

The icons are hand-drawn vector marks. They use no icon library, no third-party artwork, no raster
images, no gradients and no shadows. Never edit the exported SVG files by hand. Change the source,
then run the export. The test fails when a file is missing, stale or extra.

## The Grid and the Stroke

- **Grid:** a 24 × 24 unit square with a 2-unit margin. Every mark stays inside the 2–22 live area,
  and the test checks this.
- **Keylines:** a full-size glyph fills about 18 × 18 units: a circle has radius 8 to 8.5, a square
  is 16 to 18 units wide, and a page is 14 × 18 units. Small markers have radius 1 to 1.75.
- **Stroke:** 1.5 units, with round caps and round joins. Rectangles have a corner radius of 1.25
  to 2. Use the same stroke weight everywhere. A dashed stroke (2 on, 2.5 off) always means
  something not yet observed: a forecast, a planned period, or an aggregate boundary.
- **Sizes:** 20 px is the bare glyph, for list rows, links and search hits. 32 px is a card's icon.
  64 px is a page header (48 px on a phone). From 28 px up, the glyph sits on a tile at two thirds
  of the tile's size.

## The Two Tones

Each glyph is drawn in ink plus one accent. Both are lake tokens, so the icons follow the light and
dark themes:

| Tone | Class | Colour |
|---|---|---|
| Ink line | `cat-line` | `currentColor`, which is `--lake-text-primary` |
| Accent line | `cat-accent` | `--cat-accent` |
| Accent dot (a solid marker, no stroke) | `cat-dot` | `--cat-accent` |
| Tint (a flat fill behind the lines) | `cat-tint` | `--cat-accent` at 20 % |
| Tile | `.cat-icon-tile` | `--lake-ai-soft`, the soft periwinkle surface |

The accent depends on the kind of entry. Methodologies use `--lake-action` (blue), for data and
intelligence. Solutions use `--lake-warning` (amber), for decisions and returns. These are the
website hero's accent colours.

Ink carries the context, such as the person, the page or the axes. The accent carries what the
method finds or what the decision acts on, such as the predicted column, the spike, the arrow back
or the forecast. A tint always sits behind the lines and is drawn first. Use at most one tinted
area per glyph.

## The Motif Dictionary

A solution borrows the motif of the methodology it is built on, so related icons look related.

| Motif | Means | Used by |
|---|---|---|
| Table, predicted column | tabular prediction | M-1, the tabular family |
| Two records joining | entity resolution | M-2 |
| Ringed points | clusters, segments | M-3 |
| A line with a dashed future and a range cone | forecasting | M-4, S-5 (as dashed bars) |
| Steps down on axes | survival, time to event | M-5 |
| A bar above a dashed control level, arrow up | uplift, treatment effect | M-6 |
| A spike in a steady line | anomaly | M-7, S-7 (in a lens) |
| A target and an arrow | next-best-action | M-8, S-11 |
| Ranked lines, the first accented | ranking, priority | M-9, S-4 |
| A speech bubble | language | M-10, the language family |
| A page with a folded corner | documents, invoices, claims | M-11, S-4, S-12, B-4 |
| Nodes and edges | graphs, networks | M-12, S-10, the identity family |
| A viewfinder and an eye | vision | M-13 |
| A waveform | speech and audio | M-14 |
| Three paths from one start | options, bandits, routing | M-15, S-12, B-3 |
| A region and its best corner | optimization | M-16 |
| A bell curve with its interval | uncertainty | M-17 |
| Stacked layers | ensembles | M-18 |
| A tag | labels, prices | M-19, B-1 |
| A shield | protection, privacy, fraud, security | M-20, S-10, B-8 |
| Waves and a loop arrow | streaming, staying fresh | M-21 |
| A person (head and shoulders) | a customer, an employee | S-1, S-3, S-11, B-2 |
| A clock | time, service due | S-8, the time family |

## When an Icon Is Missing

`solutionIcon` and `methodologyIcon` in `catalogueIcons.ts` pick an icon for any entry. A new
catalogue entry or a workspace's own solution never shows a blank space.

1. A catalogue entry uses its own glyph.
2. A solution of your own that was duplicated from a catalogue solution uses the original's glyph
   (`duplicated_from`).
3. Otherwise, a solution of your own uses the glyph of its primary methodology: the one marked
   `primary`, or else the first one listed. It is drawn with the solution's amber accent.
4. A methodology that the set has not drawn uses the glyph of its engine family. There are six
   family glyphs: table cells, a tree of records, a clock, a signpost, a speech bubble and a
   database.
5. If none of these applies, the icon is a monogram: the first letter or digit of the name, in the
   accent colour on the tile.

## Drawing a New One

1. Add the entry to `METHODOLOGY_GLYPHS` or `SOLUTION_GLYPHS`, with its name in `CATALOGUE_NAMES`.
   Build the glyph from the shared helpers (`person`, `shield`, `page`) and the motifs above.
2. Give it a `label` that describes the picture (for example, "A person, and an arrow turning back
   towards them"). The label is the icon's accessible name and its tooltip. It must not repeat
   another glyph's label.
3. Add the new id to the test's catalogue list. Run `node tools/ui/export-catalogue-icons.mjs` and
   then `npm test`.
4. Look at it on `/design/icons` at 20, 32 and 64 px in both themes. At 20 px it must still read.
   If it does not, remove detail rather than thinning the stroke.

## Accessibility

Each icon has `role="img"` and an `aria-label` that describes the picture. Its `title` gives the
same text as a tooltip. The name of the entry stays in the card's or link's own text, so the icon
never carries meaning by itself. Each icon is drawn in at least two tones, so the ink lines alone
still show the shape in high-contrast modes. The accents use the action and warning tokens, which
meet 3:1 against the tile in both themes.
