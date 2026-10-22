# Train → Streams

The Streams list (`/train/streams`) and one topic's message browser (`/train/stream`): browsing,
Live, publishing, editing and deleting messages, purging, topic settings and deleting a topic, with
what each persona may see. Source: `frontend/src/pages/streams/`.

## Before you start

- Personas: `owner` (admin on the account), `writer`, `member`, `operator`.
- Test data: `W1`; `TD-AGENT` (the frontier agent `qa-<MMDD>-frontier-agent` from
  [10-design.md](10-design.md)).
- The system topic `agent-events` in `W1` holds messages once any agent turn has run (the Agent Factory
  and Project Agent cases in 10-design.md make some). Note one task id from those turns.
- A second browser profile signed in as `owner`, to publish while the first one watches.
- Names made here: topic `qa-<MMDD>-orders`.
- Unlike Design, these pages read `my_permissions` on the account that owns the workspace (`write`,
  `admin`) and the topic's `can_manage`, and hide the controls a persona may not use. The capability
  checks again.

## Streams list

#### STRM-001 · The Streams list · P0
As: owner

1. In `W1`, open **Train → Streams**.

Expect:
- Title "Streams", eyebrow "Train", sub "Browse retained messages without consuming them. Live reads
  leave subscriber positions unchanged."; breadcrumb Train › Streams; header **New topic**.
- A `list_streams` request with `customer_id`, `workspace_id`, `limit=50`.
- Figures **Topics**, **Held messages**, **Subscribers**.
- One card per place, label "Topics", titled "Workspace" and "Lake Bus", each with its topic count.
- Columns **Topic**, **Subscribers**, **Held Messages**, **Next Sequence**, each sortable.

#### STRM-002 · Topic names, labels and counts · P1
As: owner · Needs: agent turns in W1

1. Read the rows of both cards.

Expect:
- `agent-events` reads "Agent events"; `agent-tasks.<agent>` reads "Agent tasks · <agent>"; when the label
  differs from the name, the raw name is below in mono, without a `topic:` prefix.
- System topics carry the chip "system".
- A topic whose counts Lake does not share (shared Agent tasks rows) shows "—" for subscribers and held
  messages, and is left out of the figures. When no topic is counted, the figures say "—".
- Each topic name opens `/train/stream?customer_id=…&zetta=workspace|bus&topic=<name>`.

#### STRM-003 · Filter by name prefix · P1
As: owner

1. Type `agent` in "Name prefix…" and press **Filter**.
2. Type `qa-none` and press **Filter**.

Expect:
- Step 1: the address has `prefix=agent`; only topics starting with `agent` are listed; reloading keeps it.
- Step 2: "No Topics" with "No topics match. Clear the prefix, or create a topic."

#### STRM-004 · Page size bounds · P2
As: owner

1. Open `/w/<W1 id>/train/streams?limit=500`, then `?limit=abc`, then `?limit=0`.
2. Type `300` in **Page size** and press **Filter**.

Expect:
- Step 1: each sends `limit=50` and shows 50 in **Page size**.
- Step 2: the browser refuses the value (maximum 200).

#### STRM-005 · Page through the topics · P2
As: owner

1. Set **Page size** to `1` and press **Filter**.
2. Press **Next**, then **Previous**.

Expect:
- **Next** shows while there are more topics; the **Topics** figure says "on this page".
- **Next** and **Previous** add `cursor=` to the address, keeping `prefix` and `limit`; **Previous**
  shows only after moving on.

## New topic

#### STRM-006 · New Topic refuses bad settings · P2
As: owner

1. Press **New topic**. Read the drawer.
2. Type **Topic name** `qa-<MMDD>-orders`, replace **Settings (JSON)** with `{retention`, press **Create topic**.

Expect:
- The drawer "New Topic", "New topics live in this workspace.", fields **Topic name** and **Settings
  (JSON)**, prefilled with `retention_ms` 86400000, `max_events` 1000 and `delivery.guarantee`
  `at_least_once`, and the note "Configure retention_ms, max_events, memory_cap_bytes, disk_cap_bytes and
  delivery defaults. Null clears an optional override."
- Step 2: "Settings: <the parser's message>" in the drawer; no `create_stream` request.

#### STRM-007 · Create a topic · P0
As: owner

1. Restore the default settings and press **Create topic**.

Expect:
- `create_stream` with `zetta: "workspace"`, the topic and the settings.
- The topic page opens at `/train/stream?customer_id=…&zetta=workspace&topic=qa-<MMDD>-orders`.
- Back on the list, the "Workspace" card has the row with no "system" chip.

#### STRM-008 · Cancel after a refused create · P2
As: owner

1. Press **New topic**, type the name `qa-<MMDD>-orders` again and press **Create topic**.
2. Press **Cancel**.
3. Repeat step 1 and close with the drawer's close button instead.

Expect:
- Step 1: Lake's refusal (the topic exists) shows in the drawer.
- Step 2: the drawer closes, but the same error shows at the top of the Streams page (suspected defect 1).
- Step 3: the drawer closes and no error stays.

## Topic page

#### STRM-009 · A new topic's page · P0
As: owner · Needs: STRM-007

1. Open `qa-<MMDD>-orders`.

Expect:
- Title `qa-<MMDD>-orders`, eyebrow "Train · Streams"; header **All streams** and **Publish message**.
- Requests `get_stream` and `browse_stream` with `direction=backward`.
- Side "Topic" card: **Lives in** "This workspace", **Kind** "Topic", **Next sequence**, **Subscribers** 0.
- "Topic Settings" with **Edit** and a disclosure "Settings" holding the settings as JSON.
- "Subscribers" says "No subscriptions."
- A danger card "Purge and Delete" (label "Danger zone") with "Purge Messages" and "Delete Topic".
- The "Messages" card: label "0 shown · newest first", the switch reading "Paused", **Jump to sequence**,
  **Newest messages**, "Paused. Turn on Live to follow new messages. Copy and edit use the displayed
  revision. Refresh to see edits to older messages.", and "No retained messages."

#### STRM-010 · Publish a message · P0
As: owner · Needs: STRM-009

1. Press **Publish message**. Replace `{}` with `{"status":` and press **Publish**.
2. Replace it with `{"status": "ready", "id": 1}` and press **Publish**.
3. Publish two more messages with `id` 2 and 3.

Expect:
- The dialog "Publish Message" (subtitle the topic) with **JSON payload**.
- Step 1: "Payload: <the parser's message>" in the dialog; no request.
- Step 2: `publish_stream_message` with the payload as a string; toast "Message published"; the dialog
  closes; "Message 1" is listed.
- If Lake answers `accepted: false`: "The workspace did not accept the message. Check the topic limits and
  overflow policy."
- After step 3 the messages are newest first and "3 shown · newest first".

#### STRM-011 · What a message row shows · P1
As: owner · Needs: STRM-010

1. Read "Message 3".
2. Publish a payload over 1,024 characters (a JSON string field of 1,100 characters).

Expect:
- "Message 3", "Sender: <who>", "Published: <relative time>" (the tooltip gives the Unix milliseconds),
  the size, "rev 1"; **Copy message**, **Edit**, **Delete message**.
- The "JSON payload" disclosure shows pretty-printed JSON and is open.
- Step 2: the new message's disclosure starts closed.

#### STRM-012 · Copy a message · P2
As: owner · Needs: STRM-010

1. Press **Copy message** on "Message 1" and paste into a text editor.

Expect:
- The pasted text is the pretty-printed payload. The button reads "Copied", and stays so (suspected defect 3).
- If the browser denies the clipboard, the error shows at the top of the page.

#### STRM-013 · Edit a message · P1
As: owner · Needs: STRM-010

1. Press **Edit** on "Message 2". Change `ready` to `done`. Leave the box unticked and press **Save payload**.

Expect:
- The dialog "Edit Message 2" with **JSON payload**, the checkbox "Re-deliver to subscribers that already
  received it" (unticked) and "Off by default. Enabling this queues the edited payload again for
  subscriptions that received this message, including those that acknowledged it. Pending subscribers
  receive the edit either way."
- `update_stream_message` with `sequence: 2`, `expected_revision: 1`, `redeliver: false`; toast "Message 2
  saved"; the row shows "rev 2" and the new payload.
- Invalid JSON gives "Payload: …" in the dialog.

#### STRM-014 · A stale edit is refused · P1
As: owner, in two browsers · Needs: STRM-013

1. In both browsers open the topic.
2. In browser B, edit "Message 2" and save.
3. In browser A (still showing the old revision), edit "Message 2" and save.

Expect:
- Step 3: Lake's `conflict` refusal shows in the dialog; nothing changes. After a reload browser A shows
  the newer revision.

#### STRM-015 · Follow the topic live `v0.4.0+` · P1
As: owner, in two browsers · Needs: STRM-009

1. In browser A, press the switch so it reads "Live".
2. In browser B, publish `{"id": 10}`, then `{"id": 11}`.
3. In browser A, press the switch again.

Expect:
- Step 1: the line says "Following new messages. Pause Live to stop."; `browse_stream` requests go forward
  with `wait_ms=10000` and `limit=100`, each waiting up to 10 seconds (on v0.3.1 `wait_ms` as query text is
  ignored, so the reads return at once and repeat).
- Step 2: each new message appears on top in browser A without a reload.
- Step 3: the switch reads "Paused" and no further `browse_stream` request is made.
- With no messages yet and Live on, the empty text reads "No retained messages yet. New ones appear here as
  they are published."

#### STRM-016 · Jump, older and newest · P1
As: owner · Needs: STRM-010

1. Type `2` in **Jump to sequence** and press **Jump**.
2. With more than 100 messages (publish them with the API if needed), press **Older messages** at the foot.
3. Press **Newest messages**.

Expect:
- Step 1: the address gets `from_sequence=2`; the list starts at message 2 going back. An empty field is
  refused by the browser.
- Step 2: **Older messages** shows only when there are more; it adds `from_sequence=<n>`.
- Step 3 returns to the newest messages; a Live switch that was on is now "Paused" (each address is a
  fresh page).

#### STRM-017 · Change the topic's settings · P1
As: owner · Needs: STRM-010

1. Press **Edit** on "Topic Settings". Read the drawer.
2. Set `max_events` to `2` and press **Preview and save settings**.
3. Press **Cancel** in the confirmation. Then press **Preview and save settings** and **Apply**.

Expect:
- The drawer "Edit Settings" with "A retention shrink removes old messages immediately. The confirmation
  shows the current trim count." and **Mutable settings (JSON)**, without the structural keys
  (`lifecycle`, `store`, `ring_capacity`, `inline_threshold`, `priority_levels`, `producer_queue`, `exposed`).
- Step 2: `update_stream` with `dry_run: true`; a dialog "Confirm Action" with "Apply these settings? This
  currently trims <n> held messages. Messages published or expired meanwhile may change the count." and
  **Cancel**, **Apply**.
- Step 3: Cancel changes nothing and keeps the drawer; **Apply** sends `update_stream` without `dry_run`,
  toasts "Settings saved", closes the drawer, and the list keeps only the newest two messages.
- Invalid JSON gives "Settings: …" in the drawer.

#### STRM-018 · An agent started by the topic · P1
As: owner · Needs: STRM-007, TD-AGENT

1. Open `qa-<MMDD>-frontier-agent` (**Design → Agents**), press **Edit**.
2. Under **Starts on new messages in**, press `qa-<MMDD>-orders` and press **Save**.
3. Publish `{"id": 20}` on `qa-<MMDD>-orders`.
4. Open the agent's **Run it** (Train → Agent, Runs tab).

Expect:
- The streams list has `qa-<MMDD>-orders` but no system topic (`agent-events` is not offered).
- Step 2: `set_agent_streams` with `["qa-<MMDD>-orders"]`; toast "Agent saved as a new revision". "Where It
  Is Used" lists `qa-<MMDD>-orders` as a link with "each new message starts a run, as <owner's name>".
- Step 4: within about 15 seconds a run started by the stream is listed. Only messages after step 2 start runs.
- The link under "Streams that start it" opens the topic page.

#### STRM-019 · A system topic is browse-only · P1
As: owner · Needs: agent turns in W1

1. Open `agent-events` from the "Workspace" card.
2. Type the noted task id in **Turn (task ID)** and press **Filter events**.
3. Press **Jump** to an older sequence, then turn **Live** on.

Expect:
- Title "Agent events" with the chip "system · browse-only"; **Kind** "System (browse-only)".
- No **Publish message**, no **Edit** on settings or messages, no **Delete message**, no danger card;
  **Copy message** stays.
- The fields **Turn (task ID)** and **Agent** and **Filter events** show only on this topic; step 2 adds
  `task=` and lists only that turn's events.
- Step 3 keeps `task=` (and `agent=`) on the jump and in the Live reads.

#### STRM-020 · View this turn in Streams · P2
As: owner · Needs: an Agent Factory conversation (DSGN-085)

1. On the Agent Factory with a `task=`, press **View this turn in Streams**.

Expect:
- The topic page opens on `agent-events` with `zetta=workspace` and `task=<task id>`, and the **Turn (task
  ID)** box filled.

#### STRM-021 · A Lake Bus topic · P2
As: owner

1. Open a topic from the "Lake Bus" card.

Expect:
- **Lives in** "Lake bus"; it is browse-only like STRM-019; only the owner's own tasks are listed.

#### STRM-022 · A topic address that names nothing · P2
As: owner

1. Open `/w/<W1 id>/train/stream`.
2. Open `/w/<W1 id>/train/stream?zetta=workspace&topic=qa-<MMDD>-none`.

Expect:
- Step 1: the page "Streams" with "No Topic Named" and "Open a topic from the list of streams." and **All streams**.
- Step 2: an error block "Couldn't load this topic" with Lake's message and **Retry** (unless it is a 404).

## Permissions

#### STRM-023 · A reader browses only · P1
As: member · Needs: STRM-010

1. Open **Train → Streams** in `W1`, then `qa-<MMDD>-orders`.

Expect:
- No **New topic** on the list.
- On the topic: messages, **Copy message**, Live, Jump and the settings disclosure work; no **Publish
  message**, **Edit**, **Delete message** or danger card.

#### STRM-024 · A writer publishes but does not delete · P1
As: writer · Needs: STRM-010

1. Open `qa-<MMDD>-orders`. Publish `{"id": 30}` and edit it.

Expect:
- **New topic**, **Publish message** and each message's **Edit** show and work.
- No **Delete message** and no danger card (both need `admin`). No **Edit** on "Topic Settings" (only the
  topic's creator sees it).
- The page decides from `my_permissions` on the account, not on `W1`. If `writer`'s grant is on `W1` only
  and the write controls are missing, record it (suspected defect 6).

#### STRM-025 · The operator · P2
As: operator

1. Open `/train/streams`.

Expect:
- The page shows "No Topics" with zero figures and sends no `list_streams` request (suspected defect 2).
  Record what is shown.

## Phone width and keyboard

#### STRM-026 · Streams at 390 px · P2
As: owner · Needs: STRM-010

1. At 390×844, open **Train → Streams**, then `qa-<MMDD>-orders`.

Expect:
- The list's table scrolls inside its card; the filter fields wrap.
- On the topic, the "Messages" card comes first; "Topic", "Topic Settings", "Subscribers" and "Purge and
  Delete" follow below it.
- A long payload scrolls inside its box; the message buttons wrap under the heading.

#### STRM-027 · Keyboard on the topic page · P2
As: owner · Needs: STRM-010

1. Tab to the Live switch and press **Space** twice.
2. Tab to **Jump to sequence**, type `1`, press **Enter**.

Expect:
- The switch is a button with `aria-pressed` that changes with each press; its tooltip reads "Follow live
  updates" or "Pause live updates".
- **Enter** in the field jumps.
- The messages list announces new messages politely while Live is on.

## Deleting

#### STRM-028 · Delete one message · P1
As: owner · Needs: STRM-010

1. Press **Delete message** on the newest message.
2. Press **Cancel**.
3. Press **Delete message** again and **Delete**.

Expect:
- `delete_stream_messages` with `sequences: [<n>]` and `dry_run: true`; the dialog "Confirm Action" says
  "Delete 1 held messages? Subscribers that have not received them will skip them." with **Cancel** and
  **Delete**.
- Step 2 changes nothing and shows no toast.
- Step 3 sends `confirm_count: 1`; toast "Messages deleted"; the row goes.

#### STRM-029 · Purge before a sequence · P1
As: owner · Needs: STRM-010

1. Under "Purge Messages", press **Preview purge** with **Before sequence (exclusive)** empty.
2. Type the newest sequence and press **Preview purge**; press **Delete**.

Expect:
- Step 1: the browser refuses the empty field.
- Step 2: a dry run with `before_sequence`; "Delete <n> held messages? Subscribers that have not received
  them will skip them."; confirming sends `confirm_count: <n>`, toasts "Messages deleted" and leaves only
  the newest message.
- A count that changed between preview and delete is refused with `conflict`; preview again.

#### STRM-030 · Delete the topic · P1
As: owner · Needs: STRM-007

1. Under "Delete Topic", read the text, type `qa-<MMDD>-order` in **Type the topic name** and press
   **Delete topic**.
2. Type `qa-<MMDD>-orders` and press **Delete topic**.

Expect:
- "This removes all held messages and closes subscriptions."
- Step 1: "Type the exact topic name to confirm deletion." at the top of the page; nothing is sent.
- Step 2: `delete_stream` with `confirm_name`; the Streams list opens without the topic. There is no
  confirmation dialog and no toast (suspected defect 4).
- "Delete Topic" shows only to the topic's creator who is also an admin.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/streams/StreamsPage.tsx:184` | The New Topic drawer's **Cancel** does not clear the error (the close button does), so it reappears on the page | STRM-008 |
| 2 | `frontend/src/pages/streams/StreamsPage.tsx:41,48`, `TopicPage.tsx:45` | The account id comes only from `space?.id`; with no space (the operator) nothing is read and the page says "No Topics" | STRM-025 |
| 3 | `frontend/src/pages/streams/TopicPage.tsx:509-521` | **Copy message** turns to "Copied" and never resets | STRM-012 |
| 4 | `frontend/src/pages/streams/TopicPage.tsx:209-218` | Delete Topic has only the typed name: no dialog and no toast, while message deletes and purges confirm with a count | STRM-030 |
| 5 | `frontend/src/pages/streams/TopicPage.tsx:147` | `info.system !== false` makes a topic browse-only when `get_stream` omits `system` | STRM-009 |
| 6 | `frontend/src/pages/apps/shared.tsx:54-67` | Controls follow `my_permissions` on the account; a grant on the workspace alone may hide controls the capability would allow | STRM-024 |
| 7 | `frontend/src/pages/streams/TopicPage.tsx:551` | Every message's Edit dialog shows the page-wide error, so an error from another action appears inside it | STRM-014 |
| 8 | `docs/ui/streams.md:6-8` | Says the cards are "Workspace Zetta" and "Lake Bus"; the page says "Workspace" (`StreamsPage.tsx:153`) | STRM-001 |
| 9 | `docs/ui/streams.md` (topic) | Says each message shows its publication time in Unix milliseconds; the page shows a relative time, with milliseconds in the tooltip | STRM-011 |
| 10 | `docs/ui/deletion.md` | Lists no Streams deletion; message, purge and topic deletes do not use its dialog | STRM-028, STRM-030 |
