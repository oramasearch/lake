# UI design tools

`screenshot.sh` renders the static visual-direction mocks at 1440×900. It uses an installed
Chromium or Google Chrome binary when available and otherwise runs the pinned Puppeteer Chromium
image documented in [`docs/design/ui/mocks/README.md`](../../docs/design/ui/mocks/README.md).

Its live mode reads a tab-separated page manifest, fetches each page from a running lake with the
manifest's session cookie, and records light and dark PNGs. `lake-ui-fixture` runs the production
HTTP server over an in-memory control plane and an on-disk libliquid repository populated with
retained versions, a two-value conflict, representative preview formats, and a build result. It
covers public pages, operator pages, identity pages, repository content, hosted application
identity, secrets, sharing decisions, an individual link detail, a recipient's shared-with-me
page, metering, audit data, and the public transparency monitor without adding a fixture route
to `lake`.

```sh
cmake --build build --target lake-ui-fixture
./build/lake-ui-fixture .cache/ui-store .cache/ui-pages.tsv [PORT]
tools/ui/screenshot.sh --base-url http://127.0.0.1:4902 \
  --live-manifest .cache/ui-pages.tsv --output-dir docs/ui/screenshots/after
```

The optional port defaults to `4902`; select another when parallel fixture runs already use it and
pass the same port in `--base-url`.

`sync-design-system.mjs` refreshes or verifies the stamped DTCG snapshot. `build-tokens.mjs`
compiles the committed token layer used by `/style.css`. `test-token-gates.sh` proves both gates
reject local changes. `check-contrast.mjs` resolves the generated theme roles, calculates the WCAG
contrast ratio for every governed text, status, focus, and boundary pair, and fails below the
documented threshold. See [`docs/ui/design-system.md`](../../docs/ui/design-system.md).
