# UI design tools

`screenshot.sh` renders the static visual-direction mocks at 1440×900. It uses an installed
Chromium or Google Chrome binary when available and otherwise runs the pinned Puppeteer Chromium
image documented in [`docs/design/ui/mocks/README.md`](../../docs/design/ui/mocks/README.md).

`sync-design-system.mjs` refreshes or verifies the stamped DTCG snapshot. `build-tokens.mjs`
compiles the committed token layer used by `/style.css`. `test-token-gates.sh` proves both gates
reject local changes. See [`docs/ui/design-system.md`](../../docs/ui/design-system.md).
