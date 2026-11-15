# Website keys in the console

lake#1152. Where a person makes, sees and revokes the browser keys a website's page holds
([../api/browser-keys.md](../api/browser-keys.md)). The pages call only the three capabilities, so
an agent can do the same.

## Where

| Page | What it does | Capabilities |
|---|---|---|
| Quickstart, step 3 ([quickstart.md](quickstart.md)) | The website's address and *Make a website key*: one click makes a key bound to the quickstart's index (and model), the workspace and that address, and shows it once, already in the code | `quickstart_snippets` with `origins` |
| Ship → Search Index, *Website Keys* (`pages/ship/IndexPage.tsx`) | Websites (one or more addresses, separated by spaces or commas) and *Make a website key*, the key shown once; the index's keys with their websites, state (active, revoked, expired), whether they also ask a model, calls a minute, last use and expiry; *Revoke* with a confirmation | `create_browser_key`, `list_browser_keys` (`index_id`), `revoke_browser_key` |

The *Search Key* card beside it still makes an ordinary API token scoped to `search_index`, for a
server or a script; the card says it lasts as long as the session that made it.

## Rules

- An address is checked as it is typed (`siteOrigin` in `pages/quickstart/quickstart.ts`):
  `https://` is added when no scheme is typed, a path is dropped, and `http://` is accepted only for
  `localhost` and `127.0.0.1`. Lake checks it again; the page only says early what Lake refuses.
- A secret is shown once and never read back: `list_browser_keys` never answers it.
- Revoking asks first, naming the websites that will stop searching.

## Test hooks

`quickstart-site`, `quickstart-create-key`, `website-key-sites`, `website-key-create`,
`website-keys`, `website-key`.

## Verify

- `frontend/tests/quickstart.test.ts` (`npm test`) covers `siteOrigin`.
- `tools/e2e/quickstart.mjs` types the test site's origin, makes the key and runs the plain web
  page code from that origin ([../../tools/e2e/README.md](../../tools/e2e/README.md)).
