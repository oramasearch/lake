# Ship pages

The Ship group's pages in the web app (`frontend/src/pages/ship/`, lake#561): Models, Search
Indexes, one Search Index, and Services. They follow the page rules in [web-app.md](web-app.md):
a header with the purpose and the primary action, figures from the reads already made, a toolbar
over each list, creation in a drawer, and confirmations that name what happens.

## What each page shows

| Page | Path | Reads | Layout |
|---|---|---|---|
| Models | `/w/<id>/ship/models` | `list_models`, `list_studies`, `model_traffic` (the last 24 hours, every minute) | Serving, one row per model (its versions grouped as on Train → Models): figures (published, calls, errors and mean latency over 24 hours); a searchable table filtered by published state, each row with the version serving, its id, its calls, error rate and latency; Publish the latest version (`expose_model`) as a button, Unpublish (`unexpose_model`) behind a confirmation; the call snippets (`/v1/predict`) in a drawer, for one model or every published one. The name opens Ship → Model; "training" opens Train → Model |
| Model | `/w/<id>/ship/model?id=…&view=…` | `list_models`, `list_studies`; `list_sessions` on Access; `model_traffic` for the version on Monitoring | Serving one version (lake#599). A split: four views kept in `view` (Inference, Access, Destinations, Monitoring) on the left; Publish or Unpublish (behind a confirmation), the facts and the link to Train → Model on the right. **Inference**: the REST and MCP endpoint, the model id, the `curl` call, and Try It, which sends the rows typed (one object or a list, prefilled with a sample row keyed by the features) to `predict` and shows the ranked rows (numbered from 1 as people count, with each row's entity; the API's `row` is from 0, lake#819) and the whole answer. A clustering model (lake#1243, `isClustering`: kind `segmentation`) predicts no column: Inference first shows **What This Model Does**, its segments from its training run's `get_run_report` (label, rows, share, what sets each apart) and the tables the run wrote (the segment assignments of every row trained on, and the profiles, each linked to Train → Dataset); Try It becomes *Place a Row in a Segment*, its sample row keyed by the columns the model groups by (each feature's `source`), and shows each row's segment and distance from `predict`'s `assignments`; an engine that cannot place a row yet (before zetta#1497) is said in words under the button. **Access**: the person's keys scoped to `predict` (or `*`), "New predict key" in a dialog (`create_token` with `scopes: ["predict"]`, the secret shown once), and what lake allows from other origins. **Destinations**: answers go back to the caller only; no app, topic or webhook receives them yet. **Monitoring**: 1 hour to 30 days of calls, errors (refused and failed) and mean latency as figures and charts, the workspace's `predict` requests, the recent minutes with calls, and drift said as not measured yet |
| Search Indexes | `/w/<id>/ship/indexes` | `list_indexes`; the lean dataset list when the drawer opens | Figures (indexes, datasets covered, fields indexed); a searchable table filtered by kind; "New index" opens a drawer (`create_index`) and goes to the new index |
| Search Index | `/w/<id>/ship/index?id=…` | `get_index`, `search` | A split: the playground (query, field and hits stay in the URL as `q`, `field`, `top_k`; each hit shows the matched row's fields in the index's order, or its row id from an engine that sends none, `hitFields` in `ship/search.ts`, lake#596) and the call snippets on the left; the facts, publishing (`expose_index`, `unexpose_index` behind a confirmation) and the search key (`create_token` with `search_index`) on the right |
| Services | `/services` | `list_service_kinds`, `list_services`, the workspace's `list_connections` | Figures (services against the limit, ready, changing, failed); a table with status chips, the operator's message, stall notes and start hints, polled every 3 seconds while a service changes; the catalog of kinds as cards with their usual start time, each opening the New Service drawer on its kind; the connection (`service_connection`) in a dialog; delete in a dialog that needs the service's name typed |

`parts.tsx` holds what the pages share: `CopyIcon`/`CopyMono` (an identifier with a copy button)
and `useOrigin` (the origin outside callers use). The model pages share `pages/train/models.ts`
(grouping versions, scores, the `predict` call, try-it rows, predict keys, traffic) and
`pages/train/modelParts.tsx` with Train → Models.

## Train or Ship

A model has two pages (lake#599). **Train → Models** is about training it: its versions, their
held-out scores over time, comparing versions, what it learned from, its pipeline steps, retraining
and promoting a version. **Ship → Models** is about serving it: calling it, who may, where its
answers go and how it is doing. Each page links to the other for the same version, and Search
opens a model's training page. A standalone model (lake#654, [train.md](train.md#a-standalone-model))
is served exactly as a project's: its source reads "trained on its own" or "brought from a file".

A model a workspace methodology trained (kind `python:W-…`, lake#690) is served by its own code. Its
Serving card says so, links the methodology and shows its server's state (`get_methodology_deployment`,
refreshed every 4 seconds until it is serving, failed or stopped: pending, building, queued, staging,
starting, serving). Publish starts the server; Try It's Predict waits until it is serving and then shows
one prediction per row (row, entity, prediction) instead of ranked rows. `methodologyOf`,
`serverState` and `predictionRows` in `pages/train/models.ts`; see
[methodologies.md](../api/methodologies.md#serving-its-model).

## Deleting a Model Version or a Search Index (lake#645)

Ship → Model has **Delete** in its header: it removes that version's trained model (`delete_model`),
confirmed by the model's name, and keeps the run that trained it and the other versions. Ship →
Search Index has **Delete** in its header (`delete_index`), confirmed by the indexed table's name;
the dataset stays. A published one stops answering. See [deletion.md](deletion.md).

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
