# Quickstart

lake#1128. The home page of a signed-in person (`/`), first in the rail's Home group, above Ask
Orama. It runs the [quickstart process](../api/quickstart.md): drop a dataset, get a search box over
it, put that search on a website, train a model on the same data, and see the model's answers beside
each result, on the page and on the website.

## What the page shows

| Card | What it does | Capabilities |
|---|---|---|
| Steps | Every step with its state (done, running, failed and why, pending) | `quickstart_status` (polled every 5 s while the workspace starts and every 3 s while the model trains) |
| Step 1: Drop a Dataset | A drop zone for a CSV, JSON or JSON Lines file of up to 16 MiB; a JSON array is sent as one line per object; Parquet is refused with what to do. Without a workspace, one is made and the page holds the file until it is ready, then starts again by itself | `start_quickstart` |
| Step 2: Search Your Data | A search box over the published index, results as you type; once the model is published, its answer beside each result | `search_index`, `predict` |
| Step 3: Search on Your Website | The website's address (`https://…`; `http://` only for localhost, checked as it is typed) and Make a website key: one click makes a browser key bound to the index (and the model), the workspace and that address, shown once, and two tabs (lake#1185): **Copy agent prompt** (the default), one block for a coding agent with the source, the ids, the key once made, what it can do, Lake's answers, the origins and how to check it ([the agent prompt](../api/quickstart.md#the-agent-prompt)); and **Source code**, the code for a plain web page, React or curl with the key, Lake's host and the ids filled in (lake#1152, [browser keys](../api/browser-keys.md)). A key made before the model was published can only search: the page says so and offers a new one. *Website keys* links to the index's page, which lists and revokes them | `quickstart_snippets` (POSTed; with `origins` it makes the key through `create_browser_key`) |
| Step 4: Train a Model on the Same Data | The column to predict, listing only columns the model can predict, each with its detected type (first pick: `suggested`); for a category, the value to look for; the column that names a row (identifiers marked; first pick: a detected identifier; *None: Lake numbers the rows*); the snapshot date (a date field, sent as `as_of` when changed); *How Lake reads your columns*, every column with its type and whether it is predicted, names a row, is learned from or is left out and why; what training costs, Train a model. A refusal is shown in plain words with the choices still open and *Train with these choices*; a dataset with nothing to predict says what column to add. The progress while it runs, then what it learned from and left out; once trained, the page publishes the model by itself, as part of the same click (lake#1181) | `train_quickstart`, `quickstart_status` |

**Start another** opens the drop zone again; **Your quickstarts** opens an earlier one. A person who
leaves and comes back sees the newest quickstart of the workspace where it stopped.

**Cost and balance.** Before a paid step (adding the dataset, training) the card says what it is
billed by, at the published rates, and the balance (`quickstart_status` with `include_costs`). While
the balance blocks paid work (`viewer.balance.state` is `blocked`, or a call answers
`payment_required`), the button is replaced by **Add funds**, which opens `/billing/add-funds`; the
step does not run and does not fail.

**Ask Orama** moved to `/ask` (its page, `pages/ask/AskPage.tsx`, the conversation that was the home
since lake#600). Old links keep working: `/?conversation=`, `/?agent=` and `/?from=` redirect to
`/ask` with their query, as does `/w/<id>/ask`; the side panel's *Open as a page* and *Talk to this
agent in Ask Orama* go to `/ask`.

## Files

- `frontend/src/pages/quickstart/QuickstartPage.tsx`: the page.
- `frontend/src/pages/quickstart/quickstart.ts`: the pure rules (a file's format, the first model's
  columns: `predictable`, `startingChoice`, `valuesOf`, `columnUses`, `trainInput`, `trainLabel`
  (lake#1181), with `firstColumns` for an engine that could not profile them, polling, publishing a trained model, the key in the code, whether the key covers the
  model, the website step's tabs and the agent prompt with the page's key (`agentPrompt`, lake#1185), the website's address as the origin a browser key lists (`siteOrigin`), the balance),
  tested by `frontend/tests/quickstart.test.ts`.
- `frontend/src/pages/ask/home.ts`: what `/` shows each viewer and where the old Ask Orama links
  go, tested by `frontend/tests/home.test.ts`.

## Test hooks

Stable `data-testid`s for walkthroughs: `quickstart-page`, `quickstart-steps`,
`quickstart-step-<step>` (with `data-state`), `quickstart-drop`, `quickstart-start`,
`quickstart-start-another`, `quickstart-choose`, `quickstart-resume`, `quickstart-search`,
`quickstart-hits`, `quickstart-hit`, `quickstart-answer`, `quickstart-website`,
`quickstart-site`, `quickstart-create-key`, `quickstart-website-views`, `quickstart-agent-prompt`, `quickstart-snippet-<html|react|curl>`, `quickstart-train`,
`quickstart-target`, `quickstart-target-value`, `quickstart-entity`, `quickstart-as-of`,
`quickstart-columns`, `quickstart-column` (with `data-use`), `quickstart-model-problem`,
`quickstart-no-target`, `quickstart-prepared`, `quickstart-train-button`, `quickstart-training`,
`quickstart-cost-<step>`, `quickstart-top-up`. `Button` passes `data-testid` through.

## Verify

`npm test` in `frontend/` (the build runs it). In a browser, `tools/e2e/quickstart.mjs` signs in
with a Clerk ticket and walks it, copies the plain web page code into a page on another origin and
searches there. On 2026-10-24 the whole page was walked against a local lake with a workspace on a
local Zetta v0.5.0: a 40-row CSV became a search box (8 hits for "space"), the plain web page code
served from another origin returned results, training and publishing finished, and both the page
and the website showed the model's answer beside each result.

## How to change it

A new step is a card here and a step in the API ([../api/quickstart.md](../api/quickstart.md#how-to-change-it));
the page reads every state from `quickstart_status`, never works one out itself.
