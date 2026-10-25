# Quickstart

lake#1128. The home page of a signed-in person (`/`), first in the rail's Home group, above Ask
Orama. It runs the [quickstart process](../api/quickstart.md): drop a dataset, get a search box over
it, put that search on a website, decide with Ask Orama what to build on the same data (lake#1182),
and, for a model, see its answers beside each result, on the page and on the website.

## What the page shows

| Card | What it does | Capabilities |
|---|---|---|
| Steps | Every step with its state (done, running, failed and why, pending) | `quickstart_status` (polled every 5 s while the workspace starts and every 3 s while the model trains) |
| Step 1: Drop a Dataset | A drop zone for a CSV, JSON or JSON Lines file of up to 16 MiB; a JSON array is sent as one line per object; Parquet is refused with what to do. Without a workspace, one is made and the page holds the file until it is ready, then starts again by itself | `start_quickstart` |
| Step 2: Search Your Data | A search box over the published index, results as you type; once the model is published, its answer beside each result | `search_index`, `predict` |
| Step 3: Search on Your Website | The website's address (`https://…`; `http://` only for localhost, checked as it is typed) and Make a website key: one click makes a browser key bound to the index (and the model), the workspace and that address, shown once, and two tabs (lake#1185): **Copy agent prompt** (the default), one block for a coding agent with the source, the ids, the key once made, what it can do, Lake's answers, the origins and how to check it ([the agent prompt](../api/quickstart.md#the-agent-prompt)); and **Source code**, the code for a plain web page, React or curl with the key, Lake's host and the ids filled in (lake#1152, [browser keys](../api/browser-keys.md)). A key made before the model was published can only search: the page says so and offers a new one. *Website keys* links to the index's page, which lists and revokes them | `quickstart_snippets` (POSTed; with `origins` it makes the key through `create_browser_key`) |
| Step 4: Decide What to Build (lake#1182) | An **Ask Orama conversation** about the dataset just added, opened by itself once search works ([below](#step-4-decide-what-to-build)). Ask Orama proposes what is worth building; the person talks it through and confirms in the chat. Confirming a model trains it and the card becomes the next row. *Choose the columns myself* is a small fallback that opens the form of the next row. **Start over** opens a new conversation | `start_quickstart_conversation`, `send_ask_message`, `get_ask_conversation`, `get_agent_task`, `settle_ask_turn`, `confirm_ask_proposal` |
| Step 4, once a model is asked for: Train a Model on the Same Data (lake#1204: one view per state, [below](#every-state-has-one-next-step)) | The column to predict, listing only columns the model can predict, each with its detected type (first pick: `suggested`); for a category, the value to look for; the column that names a row (identifiers marked; first pick: a detected identifier; *None: Lake numbers the rows*); the snapshot date (a date field, sent as `as_of` when changed); *How Lake reads your columns*, every column with its type and whether it is predicted, names a row, is learned from or is left out and why; what training costs, Train a model. A failure is shown in plain words with **Try again** and **Ask Orama to fix it**, and the choices under *Change the choices* with *Train with these choices*; a dataset with no column to pick says Ask Orama can derive one or group the rows with clustering, never a dead end. A target derived in the conversation is shown by its rule (*Spend above 15*). A clustering model (M-3) says it is trained, offers **See the segments** (the model's page in Train), and is not added to the website. The progress while it runs, then what it learned from and left out; once trained, the page publishes the model by itself, as part of the same click (lake#1181) | `train_quickstart`, `quickstart_status` |

## Step 4: Decide What to Build

lake#1182, approved by Steve on 2026-10-25. Before it, step 4 guessed a column to predict and ended
in *Nothing to Predict Yet* when there was none. Now it is a conversation.

- **It opens by itself.** Once the search index is published, nothing is training and the balance
  allows it, the page calls `start_quickstart_conversation` once. The person's first message is
  *"I just added the dataset `<name>`. What could I build with it? Which ideas are the most
  interesting?"*, and every message carries the quickstart's context as `page`
  (`conversation.page`: the dataset, its row count, each column with its detected kind and whether
  it can be predicted, the search index). So Ask Orama starts from "here is what you could build
  with this dataset", never "which dataset?".
- **It is the Ask Orama conversation**, the same component as `/ask` (`AskMessages`,
  `AskComposer` and `useConversation` from `pages/ask/conversation.tsx`, given the quickstart's
  workspace as `scope`). Proposals are its confirm cards: Lake states the exact call; nothing runs
  before **Confirm**.
- **What it can lead to**: a model (`train_quickstart`, with a target derived from a column such as
  a price threshold, or clustering with M-3); opportunities from the Auto-Suggest Agent; an app such as an
  analysis dashboard from the Application Agent; an analysis in the conversation; or a generated
  corpus or labels first, cost shown before it runs. A confirmed proposal that is not a model links
  where it lives (Ship → Apps, Design → Opportunities, Data → Datasets, Design → Projects;
  `whereItLives`).
- **Confirming a model** runs `train_quickstart` with what was agreed. The page reads the status
  again and the card becomes *Train a Model on the Same Data* (*Group the Rows into Segments* for
  clustering) at once, brought into view, with the conversation folded below it. It moves on
  whenever a model was asked for, including when the engine refused to start it (lake#1204: on
  2026-10-25 a refused clustering left the page on the conversation with Ask Orama saying *"Done:
  train_quickstart."*). The conversation says what started and what comes next instead
  ([confirm](../api/quickstart.md#after-a-confirm)).
- A blocked balance shows **Add funds** instead of opening the conversation.

## Every state has one next step

lake#1204 (Steve, 2026-10-25: *"I confirmed, but it left me asking, 'what next?'"*). Each state of
the Quickstart says, for a person, what is happening and what to do; no capability names and no
engine errors (the engine's reason is kept in the error's `detail` for an agent, never shown).

| State | What the page shows | The next action |
|---|---|---|
| No dataset | Step 1's drop zone and what it costs | *Add it and build search* |
| The workspace is starting | *Making your workspace*, its phase; the file is held | Nothing: it goes on by itself |
| Uploading and indexing | The button's progress | Nothing: the call answers when search works |
| Search not built yet (left part way) | *Build the Search Index*: the dataset is in, its index is not | *Build the search index* (`start_quickstart` again) |
| Dataset or index failed | *Resume the Quickstart* with the step's reason | *Try the failed step again* |
| Search works | Steps 2 and 3 (search here, the website's key and code) and step 4's conversation | Make a website key; talk to Ask Orama and **Confirm** a proposal |
| Training (`data-view="training"`) | What trains (*a clustering model*, *a model that predicts churned*), on which dataset, for how long so far, that the page follows it, and what happens when it finishes | Nothing: the page polls `quickstart_status` every 3 s |
| Failed (`data-view="failed"`) | Why, in plain words (*Clustering needs at least 2 rows …*) | **Try again** (`train_quickstart` with nothing sent: the same choices, kept even when the engine refused to start), **Ask Orama to fix it** (the reason goes into the conversation, opened first if the person chose the columns themselves, which proposes the corrected change as a card); for a model with a column to predict, *Change the choices* opens the form |
| Trained, publishing | *Trained. Publishing it for your website…* | Nothing: the page publishes it |
| Done (`data-view="done"`) | Published: search and the website code show its answers | Copy the agent prompt or code from the website step; Ship → Models |
| Clustering done (`data-view="clustered"`) | The rows are grouped into segments; not added to the website (step 6 is *skipped* with why) | **See the segments**: the model's page in Train |
| Balance blocked | *Add funds to continue* | **Add funds**; the Quickstart goes on from where it stopped |

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
  model, when step 4 opens its conversation (`opensConversation`), where a confirmed proposal's
  result lives (`whereItLives`) and the step's title (`modelStepTitle`, lake#1182), the model step's view and when it replaces the conversation (`modelView`, `showsTraining`), how long training has run (`elapsed`), the failure's words and the message *Ask Orama to fix it* sends (`failureReason`, `fixMessage`), whether a methodology is clustering (`isClustering`, lake#1204), the website step's tabs and the agent prompt with the page's key (`agentPrompt`, lake#1185), the website's address as the origin a browser key lists (`siteOrigin`), the balance),
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
`quickstart-cost-<step>`, `quickstart-top-up`; step 4's conversation (lake#1182):
`quickstart-model-step`, `quickstart-conversation` (with `data-conversation`),
`quickstart-conversation-opening`, `quickstart-conversation-open`, `quickstart-conversation-restart`,
`quickstart-conversation-done`, `quickstart-choose-columns`, `quickstart-clustered`; each state
(lake#1204): `quickstart-train-card`, `quickstart-train` (with `data-view`: choose, training,
publishing, failed, done, clustered), `quickstart-failed`, `quickstart-try-again`,
`quickstart-ask-fix`, `quickstart-change-choices`, `quickstart-publishing`, `quickstart-done`,
`quickstart-see-segments`, `quickstart-resume-why`. `Button` passes `data-testid` through.

## Verify

`npm test` in `frontend/` (the build runs it). In a browser, `tools/e2e/quickstart.mjs` signs in
with a Clerk ticket, tops up a blocked balance on Stripe's test Checkout (`lib.ensureFunds`), walks
it, copies the plain web page code into a page on another origin and searches there, then drives
step 4's conversation: it waits for Ask Orama's answer, asks for a model when none was proposed,
confirms the `train_quickstart` proposal, checks the card moved to the training at once and the
conversation did not say *Done: train_quickstart*, and follows it to the model's answers; then it
starts another quickstart with a one-row file, asks for clustering, confirms it, and checks the page
shows the reason in plain words with **Try again** and **Ask Orama to fix it** (lake#1204). On 2026-10-24 the whole page was walked against a local lake with a workspace on a
local Zetta v0.5.0: a 40-row CSV became a search box (8 hits for "space"), the plain web page code
served from another origin returned results, training and publishing finished, and both the page
and the website showed the model's answer beside each result.

## How to change it

A new step is a card here and a step in the API ([../api/quickstart.md](../api/quickstart.md#how-to-change-it));
the page reads every state from `quickstart_status`, never works one out itself.
