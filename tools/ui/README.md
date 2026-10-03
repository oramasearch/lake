# UI design tools

`screenshot.sh` renders the static visual-direction mocks at 1440×900 with the pinned Puppeteer
Chromium image documented in [`docs/design/ui/mocks/README.md`](../../docs/design/ui/mocks/README.md).
Live captures give each page 30 seconds to reach DOM-ready and network-idle, save a partial image
on a timeout, and report all timed-out pages after finishing the manifest.

Its live mode reads a tab-separated page manifest, fetches each page from a running lake with the
manifest's session cookie, rewrites the content-hashed stylesheet URL to the fixture server's
origin, and records light and dark PNGs. `lake-ui-fixture` runs the production HTTP server over an
in-memory control plane and an on-disk libliquid repository populated with retained versions, a
two-value conflict, representative preview formats, and a build result. It covers public pages,
operator pages, identity pages, repository content, hosted application identity, secrets, sharing
decisions, an individual link detail, a recipient's shared-with-me page, metering, audit data, and
the public transparency monitor without adding a fixture route to `lake`. Its application lives in
one Ready workspace whose record store is the tests' `RecordTables` fake, so the event log page
reads its event from the workspace as production does (lake#356). Its application user and
organization are records in that workspace too (lake#353, lake#354).
reads its event from the workspace as production does (lake#356), and its signing key ring is in
the same workspace (lake#352), as are its settings, which the hosted pages render (lake#351).
reads its event from the workspace as production does (lake#356). Its secret mount is in the same
workspace, written through the secrets API's workspace records (lake#328), and the secret-mounts
page is captured with that workspace's `workspace_id`.

```sh
cmake --build build --target lake-ui-fixture
./build/lake-ui-fixture .cache/ui-store .cache/ui-pages.tsv [PORT]
tools/ui/screenshot.sh --base-url http://127.0.0.1:4902 \
  --live-manifest .cache/ui-pages.tsv --output-dir docs/ui/screenshots/after
```

The fixture also writes `<manifest>.routes`: every GET route the server registered, with its
workspace parameter set to the `research` workspace and any other parameter set to `fixture`.
`test-fixture-pages.sh PATH_TO_LAKE_UI_FIXTURE` (ctest `lake-ui-fixture-pages`, lake#497) starts
the fixture on a free port, requires each manifest page to answer 200 with HTML and every
registered route to answer below 500 signed in, as the operator, and signed out, and requires
the fixture to still be running at the end. The individual customer is the one password sign-up
provisions; creating a second one is refused, which is the startup failure lake#497 fixed.

The optional port defaults to `4902`; select another when parallel fixture runs already use it and
pass the same port in `--base-url`.

`sync-design-system.mjs` refreshes or verifies the stamped DTCG snapshot. `build-tokens.mjs`
compiles the committed token layer used by the content-hashed stylesheet and its `/style.css`
compatibility alias. `test-token-gates.sh` proves both gates reject local changes.
`check-contrast.mjs` resolves the generated theme roles, calculates the WCAG contrast ratio for
every governed text, status, focus, and boundary pair, and fails below the documented threshold.
See [`docs/ui/design-system.md`](../../docs/ui/design-system.md).

The `shared-link` page includes `docs/nested/hello.txt` to exercise folder traversal, preview,
and download without a session. Its link lives in a workspace of the organization (lake#350); the
fixture's workspace Zetta serves only lake's records, from the tests' `RecordTables`. Its links stay under `/share/<token>` and call the same recipient
capabilities as REST and MCP. See [sharing](../../docs/ui/sharing.md) for the delivery checks.

Each fixture repository belongs to a workspace of its space (lake#315): `lake-design` to the
organization's `research` workspace (the one that holds the shared link) and `private-notes` to the
individual's `personal` workspace. `lake-design`'s issues, reviews, release, webhook, export and
branch protection rule are seeded into the `research` workspace's records (lake#341).
