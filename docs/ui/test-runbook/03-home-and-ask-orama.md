# Home and Ask Orama

The Home group: the landing a visitor and the operator see at `/`, the Dashboard (`/dashboard`),
and Ask Orama, which is a signed-in person's home at `/` and the panel beside every other page.
Also the message box's microphone (dictation) and *From your phone*.

## Before you start

- Personas: `owner`, `newcomer` (still with no workspace: do not press **Start a workspace** here;
  [WS-003](04-workspaces.md) does it), `visitor`, `operator`.
- Data: W1 and `TD-W2`, `TD-ORGWS`, `TD-DATASET`, `TD-PROJECT` (one failed and one succeeded run),
  `TD-REPO`.
- `local` for the landing without Clerk (HOME-002).
- A microphone for ASK-029 to ASK-031, on a Lake with speech-to-text configured; a Lake without it
  for ASK-032.
- Ask Orama turns run on the bus and take a while; wait for each to finish before the next step.
- Start with an empty conversation: in W1, press **New conversation** if one is open.

## Landing (visitor and operator)

#### HOME-001 · The landing for a visitor under Clerk · P0
As: visitor · Needs: a Clerk Lake

1. Open `/`.

Expect:
- Breadcrumb Home › Welcome to Lake.
- Eyebrow "Lake by Orama", title "Design, train, and ship AI on your own data.", sub "Load your
  data into your own workspace, train models on it, and call them from your code, in Orama's cloud
  or your own."
- Buttons **Sign in** (primary, → `/sign-in`) and **Read the docs** (→ `/docs`), then "Accounts are
  added by invitation while Lake is in development."
- Three cards: "Design" ("Start from the business question and the decision it should improve. A
  project turns it into a pipeline you can read, change and approve."), "Train" ("Your data lives
  in your workspace: upload a file, connect a database or generate a dataset, then train on it and
  read what each run found.") and "Ship" ("Publish a model and call it from your code, publish a
  search index, or run databases for your team. Files and code live in repositories."), with no
  links inside them.

#### HOME-002 · The landing without Clerk · P2
As: visitor · Needs: `local`

1. Open `/`.

Expect:
- Buttons **Create account** (primary, → `/sign-up`), **Sign in** and **Read the docs**; no
  invitation sentence.

#### HOME-003 · The landing for the operator · P1
As: operator

1. Open `/`.
2. Press each link in the cards.

Expect:
- Step 1: buttons **Getting started** (primary, → `/docs`) and **Load your data** (→ `/train/data`).
- Cards link "Projects"; "Data" · "Runs"; "Models" · "Repositories". No "Create a token" (see
  Suspected defects 1).
- Step 2: each opens its page (workspace pages under the operator's current workspace).

#### HOME-004 · An old Command Center link opens the Dashboard · P2
As: owner · Needs: W1

1. Open `/?ws=<W1 id>`.

Expect:
- The address becomes `/dashboard?ws=<W1 id>` and only W1 is selected.

## Dashboard

#### HOME-005 · Open the Dashboard · P0
As: owner · Needs: W1, TD-W2, TD-PROJECT

1. Press **Home → Dashboard** in the rail.

Expect:
- Breadcrumb Home › Dashboard. Eyebrow "Home", title "Dashboard", sub "Everything in Lake at a
  glance: what you have, what is running, and what needs you."
- Actions **Usage**, **Load data** and **New project** (primary).
- A "Workspaces" row of chips: "All workspaces" with the count, then one chip per workspace.
- Ten tiles: "Repositories", "Datasets", "Transcripts", "Projects", "Agents", "Runs", "Models",
  "Search Indexes", "Apps", "Services". Each shows "…" while loading, then a number or "—".
- Cards "Needs attention", "By workspace", "Recent activity" and "Quick Launch".

#### HOME-006 · Choose which workspaces count · P1
As: owner · Needs: W1, TD-W2

1. Press the W1 chip.
2. Press the W2 chip.
3. Press the W1 chip, then the W2 chip.
4. Reload. Then open `/dashboard` in a new tab.

Expect:
- Step 1: only W1 is ticked; "All workspaces" is not; the address holds `?ws=<W1 id>`; the tiles
  and the By workspace table count W1 only.
- Step 2: W1 and W2 are ticked; with every workspace chosen the address drops `ws`.
- Step 3: removing the last one goes back to "All workspaces".
- Step 4: the choice is kept (`localStorage["lake-home-workspaces"]`).

#### HOME-007 · The tiles open their pages · P1
As: owner

1. Press each tile in turn.

Expect:
- Repositories → `/repositories`; Datasets → Datasets; Transcripts → Transcripts; Projects →
  Projects; Agents → Design → Agents; Runs → Train → Runs; Models → Ship → Models; Search Indexes →
  Ship → Search Indexes; Apps → Ship → Apps; Services → `/services`.
- The Runs tile reads "<n> active · <n> failed" under its number and has a red left edge when a
  run failed. The Apps tile reads "<n> ready" when there are apps.

#### HOME-008 · Needs attention · P1
As: owner · Needs: TD-PROJECT (a failed run)

1. Read the "Needs attention" card.
2. Press **Open →** on the failed run.

Expect:
- The card's label counts its rows ("Needs attention (1)") and has the exception edge.
- The run's row reads "Run of <pipeline> in <project> failed" with its error or time beneath.
- Step 2 opens `/train/run?id=<run id>` (under the workspace).
- With nothing to show: "Nothing needs you. Every workspace is ready and nothing has failed."

#### HOME-009 · A workspace that is not ready is flagged · P2
As: owner · Needs: a workspace being prepared. None exists yet in run order: run this case right after [WS-008](04-workspaces.md) creates one

1. Open the Dashboard while the new workspace is still being prepared.

Expect:
- "Needs attention" lists "Workspace <name> is not ready" with its phase and **Open →**.
- In "By workspace", its Status is the phase and its counts are "—".

#### HOME-010 · By workspace and Recent activity · P1
As: owner · Needs: W1, TD-W2

1. Read the "By workspace" table and press a workspace name.
2. Read "Recent activity" and press **All activity →**.

Expect:
- Step 1: columns "Workspace", "Status", "Datasets", "Projects", "Agents", "Runs", "Models",
  "Apps"; a Ready workspace's Status is "Ready". The name opens `/w/<id>/design/projects`.
- Step 2: up to 12 rows, each a relative time, the capability in words and its outcome; **All
  activity →** opens `/activity`. With none: "Nothing yet."

#### HOME-011 · Recent activity cannot be read · P2
As: owner

1. In the Network panel, block `my_audit`, then reload the Dashboard.

Expect:
- "Your activity could not be read." in the card; the rest of the page fills in.

#### HOME-012 · Quick Launch · P2
As: owner

1. Press each Quick Launch button.

Expect:
- "Load data" → Datasets; "Add a transcript" → Transcripts; "See opportunities" → Opportunities;
  "Design an agent" → Design → Agents; "Ship an app" → Ship → Apps; "Search everything" →
  `/search`.

#### HOME-013 · The Dashboard with no workspace · P1
As: newcomer

1. Open `/dashboard`.

Expect:
- An empty state "Start your first workspace" / "A workspace holds your data, projects, agents and
  models. Create one to begin." with **Workspaces** (→ `/workspaces`).
- No chips, tiles or cards.

#### HOME-014 · The Dashboard on a phone · P2
As: owner

1. At 390 px, open the Dashboard.

Expect:
- Tiles in two columns; the cards stack; the By workspace table scrolls inside its card.

## Ask Orama home

#### ASK-001 · The empty Ask Orama home · P0
As: owner · Needs: W1 with TD-DATASET and TD-PROJECT

1. Press **Home → Ask Orama** in the rail, with W1 current and no conversation open.

Expect:
- Breadcrumb Home › Ask Orama. Eyebrow "Ask Orama"; heading "Hello, <first name>. What would you
  like to know?"; the badge "AI sovereignty"; "When you're tired of renting someone else's AI,
  Orama Lake is where you own your AI."; and the paragraph beginning "Your data and everything
  built from it stay in a workspace of your own."
- "Start with <W1 name>" with four placeholders, then up to four question buttons.
- "Agents you can talk to": one card per running agent with its icon, name and what it does.
- "Design, train and ship production-grade AI" with "Self-serve, real AI, trained on your own
  data." and groups such as "Data Your data, kept in your own workspace.", each listing pages with a
  one-line purpose and a sparkle question.
- At the foot: **Talk to**, the **Message** box (placeholder "What should I look at first?"), the
  microphone, "From your phone" and **Send**.

#### ASK-002 · The greeting · P2
As: owner, then a persona whose display name is empty or an email

1. Read the heading on the empty home.

Expect:
- With a display name: "Hello, <first word of it>. What would you like to know?"
- Without one, or when it looks like an email: "What would you like to know?"

#### ASK-003 · Starting questions come from the workspace · P1
As: owner · Needs: TD-DATASET, TD-PROJECT

1. Read the question buttons.
2. Press one.
3. Press the same one again after editing the box.

Expect:
- Step 1: the first reads "Why did the latest run of <pipeline> fail?"; others follow the
  workspace, e.g. "What is in the qa_<MMDD>_accounts dataset, and what could I predict from it?";
  at most four, none repeated.
- Step 2: the question is in the message box with the cursor in it; nothing is sent.
- Step 3: the box is filled with the question again.
- In an empty workspace (W2) the questions include "How do I load my first dataset?" and "Help me
  turn a business question into a project."

#### ASK-004 · What the map lists · P1
As: owner

1. Read the map's groups and items.
2. Press a page link, go back, and press a sparkle question.

Expect:
- Groups only from Data, Design, Train, Ship, Network, Organization and Developer, each with its
  line ("Production-grade AI, put to work.", "Every page is also an API." …).
- Design items are named "AI Projects", "AI Solutions", "AI Methodologies", "AI Agents"; Developer
  also lists "MCP Tools" and "MCP Worked Flows".
- "Billing" shows only when `server_info.billing` is true; "Deployments" is missing when
  `server_info.deployments` is false; "Opportunities" shows only while the Auto-Suggest Agent runs.
- Step 2: a link opens its page (workspace pages under `/w/<W1 id>/`); a question goes into the box.

#### ASK-005 · Pick an agent to talk to · P1
As: owner

1. Press an agent card other than Ask Orama.
2. Read **Talk to** under the box and open its list.
3. Expand "Where you meet it" when it shows.

Expect:
- Step 1: the card is pressed (`aria-pressed`), a new conversation starts with that agent, and the
  cursor is in the box.
- Step 2: **Talk to** shows that agent; its purpose sits under the select. Agents this Lake does not
  run read "<name> (not running on this Lake)" and cannot be chosen.
- Step 3: the text says where that agent is met in Lake.

#### ASK-006 · Send a question · P0
As: owner · Needs: W1

1. Choose **Talk to** Ask Orama. Type `What is in this workspace?` and press **Send**.
2. Watch until the answer arrives.
3. Press **View this turn in Streams**.

Expect:
- Step 1: the welcome is replaced by the conversation: an "Ask Orama" bar with **New
  conversation**, then "You" with the question. **Send** spins.
- Step 2: a working note "Waiting for Ask Orama to start…" then "Ask Orama is working…" and " · for
  <duration>", with "It keeps working if you leave; this page checks again every 2 seconds.". Then
  an answer headed "Ask Orama", rendered as Markdown. **Delete** appears beside **New
  conversation**.
- Step 3: Train → Streams opens on topic `agent-events` filtered to that task.

#### ASK-007 · Enter sends, Shift+Enter adds a line · P1
As: owner

1. On a desktop, type a line, press Shift+Enter, type a second line, then press Enter.
2. Press **Send** with an empty or blank box.

Expect:
- Step 1: the message is sent with both lines.
- Step 2: nothing is sent.

#### ASK-008 · The conversation is kept for the tab · P1
As: owner · Needs: ASK-006

1. Open another page, then come back to `/`.
2. Reload.
3. Open `/` in a new tab.

Expect:
- Steps 1 and 2: the same conversation shows.
- Step 3: the new tab starts empty (`sessionStorage`, one per tab).

#### ASK-009 · Each workspace has its own conversation · P1
As: owner · Needs: TD-W2

1. Switch to W2 with the picker, then open `/`.
2. Switch back to W1.

Expect:
- Step 1: W2's own conversation (empty the first time) with "Start with <W2 name>".
- Step 2: W1's conversation is back.

#### ASK-010 · New conversation · P1
As: owner

1. With a conversation open, press **New conversation**.

Expect:
- The welcome comes back; **Talk to** keeps the agent; the old conversation is not deleted.

#### ASK-011 · Confirm a proposal · P1
As: owner · Needs: W1

1. Ask `Create a private repository named qa-<MMDD>-ask in this workspace.` and wait for the answer.
2. Press **Confirm**.
3. Reload.

Expect:
- Step 1: under the answer a box "Proposed: `create_repository`" with its impact and **Confirm**.
- Step 2: **Confirm** spins (every other Confirm is disabled meanwhile); the conversation adds the
  outcome and the box shows the badge "Confirmed".
- Step 3: "Confirmed" stays and no Confirm is offered. The repository exists on **Data →
  Repositories**.

#### ASK-012 · A refused proposal · P2
As: owner · Needs: TD-REPO

1. Ask `Create a private repository named qa-<MMDD>-repo.` (a name already taken) and press
   **Confirm** on the proposal.

Expect:
- The proposal shows Lake's refusal in red under it.
- When Lake says the input itself was refused, no **Try again** is offered; when it says it may
  pass later, the button reads **Try again**.

#### ASK-013 · Delete a conversation · P0
As: owner · Needs: ASK-011's conversation

1. Press **Delete** beside **New conversation**.
2. Type `Delete` and read the button.
3. Clear it, type `delete`, and press **Delete conversation**.

Expect:
- Step 1: a dialog "Delete This Conversation" with "Its messages, and the agent tasks it started,
  are removed from this workspace. It cannot be undone." and a list: "<n> messages", "It starts:
  “Create a private repository named …”" and, when any, "<n> agent tasks it started, with what was
  asked and answered".
- Step 2: **Delete conversation** stays disabled (the text must match exactly).
- Step 3: the toast "Conversation deleted"; the home shows a new empty conversation.

#### ASK-014 · Old Ask links open the home · P2
As: owner · Needs: W1, TD-W2

1. Open `/ask`.
2. Open `/w/<W2 id>/ask?customer_id=x&agent=agent-ask-orama`.

Expect:
- Step 1: the address becomes `/`.
- Step 2: the address becomes `/` (no `customer_id`, no `agent`), the workspace is W2, and a new
  conversation with Ask Orama is open.

#### ASK-015 · A link to a conversation · P2
As: owner · Needs: a conversation id from ASK-006 (from `sessionStorage["lake-ask-panel:<W1 id>"]`)

1. Press **New conversation**, then open `/?conversation=<that id>`.

Expect:
- That conversation shows, and the address becomes `/` (see Suspected defects 4).

#### ASK-016 · The top bar's Ask Orama on the home · P2
As: owner

1. On `/`, put the cursor elsewhere and press **Ask Orama** in the top bar.

Expect:
- No panel opens; the cursor goes to the message box.

## Ask Orama panel

#### ASK-017 · Open the panel beside a page · P0
As: owner · Needs: W1

1. Open `/w/<W1 id>/design/projects` and press **Ask Orama** in the top bar.

Expect:
- A panel "Ask Orama" with "About the page /design/projects" under its heading, the same
  conversation as the home, and the message box at its foot.
- Head buttons: **Start a new conversation**, **Open the conversation as a page**, **Expand over
  the page**, **Close Ask Orama**. The top bar button is pressed (`aria-pressed`).
- At 1440 px wide or more the page moves aside; narrower, the panel lies over the page.

#### ASK-018 · Resize the panel · P1
As: owner

1. Drag the panel's left edge to the left.
2. Focus the edge (Tab) and press ← twice, then → once.
3. Double-click the edge.
4. Reload and open the panel again.

Expect:
- Step 1: the panel widens; it stops at 70% of the window and at 320 px when narrowed.
- Step 2: ← widens by 24 px each time, → narrows by 24 px. The edge's name is "Resize Ask Orama".
- Step 3: the width goes back to 560 px.
- Step 4: the last width is kept (`localStorage["lake-ask-width"]`).

#### ASK-019 · Expand over the page · P1
As: owner

1. Press **Expand over the page**.
2. Press **Back to the side**.
3. Expand again and close the panel; open it again.

Expect:
- Step 1: the panel covers everything but the rail; the button now reads "Back to the side"; the
  edge cannot be dragged.
- Step 2: it returns to its width at the side.
- Step 3: it opens at the side, not expanded.

#### ASK-020 · Open as a page · P1
As: owner

1. In the panel on `/w/<W1 id>/design/projects`, press **Open the conversation as a page**.
2. Press **Ask without this page**.

Expect:
- Step 1: the home opens at `/?from=%2Fdesign%2Fprojects` with the same conversation; the panel
  closes; above the box "About the page /design/projects" (linked).
- Step 2: the line goes and the address is `/`.

#### ASK-021 · Ask from a page · P1
As: owner

1. In the panel on Design → Projects, ask `What is this page?`.
2. Press **Start a new conversation**.
3. Press **Close Ask Orama**, then open it again.

Expect:
- Step 1: the answer speaks about the Projects page (the turn carried the page).
- Step 2: the conversation empties, keeping **Talk to**.
- Step 3: the panel opens with the same conversation as before closing.

#### ASK-022 · The panel with no workspace · P1
As: newcomer

1. On `/repositories`, press **Ask Orama**.

Expect:
- The panel says "Ask Orama works in a workspace, and you have none selected." and "Choose or start
  one on Workspaces." with the link to `/workspaces`, and no message box.
- The head has only **Expand over the page** and **Close Ask Orama** (no **Start a new
  conversation**, no **Open the conversation as a page**).

#### ASK-023 · The operator has no Ask Orama · P2
As: operator

1. Read the top bar on any page.

Expect:
- No **Ask Orama** button; `/` is the landing.

## Ask Orama with no workspace

#### ASK-024 · The newcomer's home · P1
As: newcomer

1. Open `/`.

Expect:
- The welcome, then "First step: start a workspace" with "Everything in Lake happens in a
  workspace: your data, projects, models and agents live there, apart from every other account's.
  Ask Orama answers about the workspace you are in.", the primary **Start a workspace** and "It
  takes a minute or two to prepare. You can read on meanwhile."
- Then "Agents you can talk to" (cards without buttons) and the map (links without questions).
- No starting questions and no message box.
- Do not press **Start a workspace** here: [WS-003](04-workspaces.md) presses it.

## Dictation and From your phone

#### ASK-025 · Dictate a message · P1
As: owner · Needs: a microphone, speech-to-text configured

1. Press **Dictate** beside the box and allow the microphone.
2. Speak a sentence, then press **Stop and transcribe**.

Expect:
- Step 1: a bar with a pulsing red dot, "Recording", a running clock, **Cancel** and the stop
  button.
- Step 2: the box's placeholder reads "Transcribing…" and the microphone spins; then the words are
  added to the box, the cursor is in it, and nothing is sent.

#### ASK-026 · Cancel a recording · P2
As: owner · Needs: a microphone

1. Press **Dictate**, speak, press **Cancel**.

Expect:
- The bar closes, nothing is sent to `transcribe`, the box is unchanged.

#### ASK-027 · A blocked microphone · P2
As: owner

1. Block the microphone for the site in the browser, then press **Dictate**.
2. Press **Dismiss** on the message.

Expect:
- Step 1: "The microphone is blocked. Allow it for this site in the browser's settings; typing and
  your keyboard's own dictation still work."
- Step 2: the message goes.

#### ASK-028 · Nothing heard · P2
As: owner · Needs: a microphone

1. Press **Dictate**, stay silent for a few seconds, press **Stop and transcribe**.

Expect:
- "Nothing was heard. Try again, closer to the microphone."

#### ASK-029 · A Lake without speech-to-text · P2
As: owner · Needs: a Lake with no speech-to-text key

1. Press **Dictate**, speak, and stop.
2. Open another page with a microphone (Data → Transcripts), and reload.

Expect:
- Step 1: "Speech-to-text is not configured on this Lake. The microphone is hidden for now; typing
  and your keyboard's own dictation still work." and the microphone disappears.
- Step 2: every dictation microphone stays hidden in this tab until it is closed
  (`sessionStorage["lake-speech-to-text"]` is `off`).

#### ASK-030 · From your phone · P2
As: owner

1. Press **From your phone** under the box.
2. Press the **Android** tab, then **iPhone**.
3. Press **Close**.

Expect:
- Step 1: a panel with tabs "iPhone" and "Android" (the one matching the device is selected; iPhone
  on a computer), three numbered steps whose last reads "open Transcripts, tap New transcript, then
  Upload audio; ask about it from its page", "Recordings: M4A or MP4, MP3, WAV, FLAC, WebM or Ogg,
  up to 25 MB. Text: TXT, Markdown, WebVTT (.vtt) or SubRip (.srt). Keep the original until the
  transcript is saved." and links "Apple: save a voice memo" and "Apple: recorded calls" that open
  a new tab.
- Step 2: the steps and links change to Android's ("Google: share a recording", "Google: recorded
  calls").
- Step 3: the panel closes.

## Phone width

#### ASK-031 · Ask Orama on a phone · P1
As: owner

1. At 390 px, open `/` and type a message.
2. On another page, press the **Ask Orama** icon.
3. In the panel, open a link inside an answer (e.g. a page it names).

Expect:
- Step 1: the questions and agent buttons are one column, each at least 44 px tall; the box sits at
  the foot; on a touch screen Return makes a new line and only **Send** sends.
- Step 2: the panel fills the screen above the shell, the page behind does not scroll, and the box
  sits above the keyboard. No resize edge and no **Expand over the page**.
- Step 3: the panel closes and the linked page shows.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/home/HomePage.tsx:94-98` | "Create a token" renders only for a signed-in non-operator, who never sees the landing, so it is never shown | HOME-003 |
| 2 | `frontend/src/pages/home/HomePage.tsx:23`, `NotFoundPage.tsx:9` | Tab titles "Lake — design, train, and ship AI" and "Lake — not found" are lowercase, against G-01 | HOME-001 |
| 3 | `frontend/src/pages/home/CommandCenter.tsx:219` | The empty state "Start your first workspace" is a heading in sentence case, against Title Case | HOME-013 |
| 4 | `docs/guide/ask-orama.md` (Limits) vs `frontend/src/pages/ask/AskHome.tsx:221-225` | The guide says the page's address names the conversation so it can be linked; the home drops `conversation` from the address | ASK-015 |
| 5 | `docs/ui/README.md` (The shell) vs `frontend/src/pages/ask/AskHome.tsx:181` | The doc names the map "What you can do with Lake"; the page heading is "Design, train and ship production-grade AI" | ASK-001 |
| 6 | `frontend/src/components/AskPanel.tsx:108` | The panel's fallback line "Ask about anything in your Lake. Changes are proposed for you to confirm." can never show: the panel always has a page path | ASK-017 |
