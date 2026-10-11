# Catalogue icons

These files are the solution and methodology icons (lake#639), one standalone SVG for each, for the
website's solutions pages. Each is in `solutions/`, `methodologies/` or `families/`.
`node tools/ui/export-catalogue-icons.mjs` writes them from `frontend/src/lib/catalogueIcons.ts`.
Do not edit them by hand.

Each file draws its ink in `currentColor`. Its accent and tint come from `--cat-accent` and
`--cat-tint`, and default to the light theme's blue (for a methodology) or amber (for a solution).
To match the app, set those two properties and the text colour on the element that holds the icon.

[`docs/ui/catalogue-icons.md`](../../docs/ui/catalogue-icons.md) has the style sheet and the
fallback rule.
