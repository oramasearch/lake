# UI tools

The console is the web app in `frontend/` ([`docs/ui/web-app.md`](../../docs/ui/web-app.md)). These
scripts build and check what it and lake's hosted application pages share, and embed the app.

`embed-frontend.mjs DIST OUTPUT` turns a built `frontend/dist` into the generated C++ translation
unit `include/lake/api/web_app.hpp` declares: every file with its content type, so the single lake
binary serves the app with no files at run time. CMake runs it after `npm run build` (or on
`-DLAKE_FRONTEND_DIST`).

`sync-design-system.mjs` refreshes or verifies the stamped DTCG snapshot of the Orama design
tokens. `build-tokens.mjs` compiles that snapshot into the committed token layer twice: as
`src/api/web_ui_tokens.inc`, the start of the content-hashed stylesheet the hosted application
pages link (`/style-<hash>.css`, with its `/style.css` compatibility alias), and as
`frontend/src/styles/tokens.css`, which the web app imports. `test-token-gates.sh` (ctest
`lake-ui-token-gates`) proves both gates reject local changes. `check-contrast.mjs` (ctest
`lake-ui-contrast`) resolves the generated theme roles, calculates the WCAG contrast ratio for
every governed text, status, focus, and boundary pair, and fails below the documented threshold.
See [`docs/ui/design-system.md`](../../docs/ui/design-system.md).

`screenshot.sh` renders the static visual-direction mocks in
[`docs/design/ui/mocks/`](../../docs/design/ui/mocks/README.md) at 1440×900 with the pinned
Puppeteer Chromium image (`screenshot_driver.mjs` runs inside it). It takes no arguments.
Screenshots of the running app come from the browser walkthrough
([`docs/deployment/browser-testing.md`](../../docs/deployment/browser-testing.md)).

lake#561 removed the tools that served the server-rendered pages: `lake-ui-fixture`
(`fixture_server.cpp`), its `test-fixture-pages.sh` check (ctest `lake-ui-fixture-pages`), and
`screenshot.sh`'s live mode, which fetched those pages.
