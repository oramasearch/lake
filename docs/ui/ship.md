# Ship pages

The Ship group's pages in the web app (`frontend/src/pages/ship/`, lake#561): Models, Search
Indexes, one Search Index, and Services. They follow the page rules in [web-app.md](web-app.md):
a header with the purpose and the primary action, figures from the reads already made, a toolbar
over each list, creation in a drawer, and confirmations that name what happens.

## What each page shows

| Page | Path | Reads | Layout |
|---|---|---|---|
| Models | `/w/<id>/ship/models` | `list_models`, `list_studies` | Figures (models, published, held-out scores, promoted); a searchable table filtered by published state and model kind; Publish (`expose_model`) as a button, Unpublish (`unexpose_model`) behind a confirmation; the call snippets (`/v1/predict`) in a drawer, for one model or every published one |
| Search Indexes | `/w/<id>/ship/indexes` | `list_indexes`; the lean dataset list when the drawer opens | Figures (indexes, datasets covered, fields indexed); a searchable table filtered by kind; "New index" opens a drawer (`create_index`) and goes to the new index |
| Search Index | `/w/<id>/ship/index?id=…` | `get_index`, `search` | A split: the playground (query, field and hits stay in the URL as `q`, `field`, `top_k`) and the call snippets on the left; the facts, publishing (`expose_index`, `unexpose_index` behind a confirmation) and the search key (`create_token` with `search_index`) on the right |
| Services | `/services` | `list_service_kinds`, `list_services`, the workspace's `list_connections` | Figures (services against the limit, ready, changing, failed); a table with status chips, the operator's message, stall notes and start hints, polled every 3 seconds while a service changes; the catalog of kinds as cards with their usual start time, each opening the New Service drawer on its kind; the connection (`service_connection`) in a dialog; delete in a dialog that needs the service's name typed |

`parts.tsx` holds what the pages share: `CopyIcon`/`CopyMono` (an identifier with a copy button)
and `useOrigin` (the origin outside callers use).

## How to verify

`npx tsc --noEmit -p .`, `npm test` and `npm run build` from `frontend/` (the build also runs
`check-effects` and `check-list-errors`). To look at the pages without a lake, serve them with
`npx vite` and answer `/v1/*` from Playwright's `page.route` with fixtures for the reads above.

## How to change

Keep the reads, actions, URL parameters and guards above: the typed name before
`delete_service`, the integer parsing of instances and storage, the `top_k` clamp to 1–100. A new
service kind needs only a row in lake's `service_kinds` (`include/lake/control/store.hpp`); add an
icon and a short description for it in `KIND_LOOK` in `ServicesPage.tsx`, or it shows a generic
server icon.
