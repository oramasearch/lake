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

## Components and pages

The shell, component catalog, page recipe, accessibility checks, and screenshot workflow are
recorded here as they are introduced. Lake keeps pure C++ renderers; the generated token layer is
committed, so neither deployment nor request handling invokes Node.
