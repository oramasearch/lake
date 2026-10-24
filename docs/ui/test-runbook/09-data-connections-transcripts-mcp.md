# Data: Connections, Transcripts and MCP Connections

This file covers three pages of the Data group: **Connections** (the databases a workspace reads),
**Transcripts** (recordings, uploads and pasted text made searchable) and **MCP Connections** (other
MCP servers the workspace is a client of), with their detail pages and the MCP sign-in callback.
All of them are workspace pages behind the workspace gate (see [04-workspaces.md](04-workspaces.md)).

## Before you start

- Personas: `owner`, `member` (read on `W1`), `writer` (write on `W1`), `newcomer`.
- Test data: `TD-PG`, `TD-SERVICE`, `TD-SECRET`, `TD-MCP`, `TD-AUDIO`, `TD-AGENT` (a frontier agent,
  for auto-approve).
- This file alone also needs:
  - `TD-PG-BAD`: the `TD-PG` host and user with a wrong password.
  - A Snowflake account with a read-only user, if one is available (otherwise mark the Snowflake case
    `blocked`).
  - A `.pdf` file and a 0-byte `empty.txt` (from `TD-FILES`), and a text file over 5 MB (from
    `TD-BIG`).
  - A browser with a microphone, on https, and permission to use it.
  - For one case, a Lake without speech-to-text (`lake-dev` with no speech key, or `local`). If none
    is available, mark that case `blocked`.
- Names: connections `qa-<MMDD>-pg`, `qa-<MMDD>-sf`; transcripts titled `qa-<MMDD>-…`; MCP
  connections `qa-<MMDD>-everything`, `qa-<MMDD>-bearer`, `qa-<MMDD>-oauth`, `qa-<MMDD>-stdio`.

## Connections

#### CONN-001 · The empty Connections page · P1
As: owner · Needs: a workspace with no connection (`W2`)

1. Switch to `W2` and open **Data → Connections**.

Expect:
- Title "Connections", eyebrow "Data". The sub ends with a link **Network → Connections**, which
  opens Network → Connections in the same workspace.
- Figures "Connections" `0`, "Kinds" `0` with "Postgres or Snowflake", "Tables reachable" `0` with
  "across every connection".
- The empty state "No connections yet" with "Add the database your data lives in, or connect one of
  your services below. Its tables can then be loaded into datasets." and **Add connection**.
- There is no search box.

#### CONN-002 · The button waits for the workspace · P2
As: newcomer

1. Open `/connections`.

Expect:
- The workspace gate shows ("No Workspace Yet" or "Start Your Workspace").
- The header has no **Add connection** button.

#### CONN-003 · Add a Postgres connection · P0
As: owner · Needs: TD-PG

1. On `W1`, open **Data → Connections** and press **Add connection**.
2. Check the drawer: title "Add a Connection", subtitle "Use a read-only account: only a single
   SELECT ever runs against it."
3. Fill **Name** `qa-<MMDD>-pg`, keep **Type** "Postgres", fill **Host**, **Port**, **User**,
   **Database**, keep **SSL mode** `require`, and fill **Password or access token**.
4. Press **Save connection**.

Expect:
- Step 3: the password field masks what is typed; under it "Sent to the workspace, which never
  returns it."
- Toast "Saved qa-<MMDD>-pg"; the drawer closes.
- The table lists `qa-<MMDD>-pg` with Kind chip "Postgres", Host `host:port/database`, Status
  "Ready" (hover: "Its credential is sealed in the workspace."), and Created "just now" or similar.
- "Tables reachable" shows a number (no "did not answer").

#### CONN-004 · Name validation · P2
As: owner

1. Open **Add connection**. Leave **Name** empty and press **Save connection**.
2. Type `qa bad name!` and press **Save connection**.
3. Type `qa-<MMDD>-pg` (the name from CONN-003), fill the Postgres fields and press **Save
   connection**.

Expect:
- Steps 1 and 2: the browser's own validation stops the form (required; pattern
  `[A-Za-z0-9_\-]{1,64}`); nothing is sent.
- Step 3: Lake's refusal shows inside the drawer; the drawer stays open; no second row appears.

#### CONN-005 · Only the chosen kind's fields are sent · P2
As: owner

1. Open **Add connection**, fill **Host** `db.example.com`, then switch **Type** to "Snowflake".
2. Check the fields, then switch back to "Postgres".

Expect:
- Snowflake shows **Account** (placeholder `ab12345.us-east-1`), **Warehouse**, **Role**, then
  **Database**, **Schema**, **Password or access token**; no **Host**, **Port**, **SSL mode** or
  **User**.
- Back on Postgres, **Host** still holds `db.example.com`.
- Press **Cancel**: nothing is saved.

#### CONN-006 · Add a Snowflake connection · P2
As: owner · Needs: a Snowflake account

1. **Add connection**, **Name** `qa-<MMDD>-sf`, **Type** "Snowflake", fill **Account**, **Warehouse**,
   **Role**, **Database**, **Schema**, **Password or access token**, and save.

Expect:
- The row's Kind chip is "Snowflake" and Host is `account/database`.
- Figure "Kinds" is `2` with "Postgres, Snowflake".

#### CONN-007 · A connection that cannot be read · P1
As: owner · Needs: TD-PG-BAD

1. Add a connection `qa-<MMDD>-pgbad` with the wrong password.
2. Read the figures, then open the connection.

Expect:
- "Tables reachable" turns red with "1 connection did not answer".
- On the connection's page, the "Tables" figure is "—" with "the database did not answer", and the
  tables area shows an error block with Lake's message.
- Delete `qa-<MMDD>-pgbad` afterwards (CONN-013 steps).

#### CONN-008 · Search the list · P2
As: owner

1. Type `pg` in **Search connections…**, then `zzz`.

Expect:
- `pg` keeps the rows whose name, kind or host contains it.
- `zzz` shows "No connection matches" with "Clear the search to see every connection."

#### CONN-009 · Connect one of your services · P1
As: owner · Needs: TD-SERVICE

1. On `W1`, scroll to the card "Connect One of Your Services".
2. Read the card, then press **Connect** beside the Postgres service.
3. Press the button again.

Expect:
- The card reads "The workspace reaches it inside the cluster, with the credential Lake already
  holds; nothing to type."
- Any service of another kind says "The workspace can't read <kind> yet; Postgres works today". A
  service still starting shows a "starting" chip.
- Step 2: toast "Connected <service name>"; a connection of the same name appears in the table; its
  Service column links to **Ship → Services**.
- Step 3: the button now reads **Refresh**; pressing it shows the toast again and adds no second row.

#### CONN-010 · A connection's page · P0
As: owner · Needs: `qa-<MMDD>-pg`

1. Press `qa-<MMDD>-pg` in the table.

Expect:
- Address `/w/<W1>/connections/connection?name=qa-<MMDD>-pg`; breadcrumb Data › Connections ›
  qa-<MMDD>-pg; title `qa-<MMDD>-pg`, eyebrow "Connection".
- Figures "Kind" (Postgres, host under it), "Tables" (a number, with "this connection can read"),
  "Status" (Ready).
- The tables list, sorted by table name: each row shows the table, its schema, a Kind chip, its
  columns as `name type, …`, and **Load into a dataset**.
- The side card lists the connector's fields and "created", and "The password or token is sealed in
  the workspace and never shown." No password appears anywhere on the page or in the Network panel.

#### CONN-011 · Load a table into a dataset · P0
As: owner

1. On `qa-<MMDD>-pg`'s page, press **Load into a dataset** on a table.
2. Check the drawer, then change **Dataset name** to `qa_<MMDD>_pulled` and fill **At most this many
   rows** `50`.
3. Press **Pull dataset**.

Expect:
- Drawer "Load into a Dataset", subtitle "One read-only SELECT against qa-<MMDD>-pg; the rows become
  a dataset you can train on." **From** is `qa-<MMDD>-pg`; **Query** is `SELECT * FROM
  <schema>.<table>`; **Dataset name** is filled from the table's name. A link "How loading data
  works" opens the Docs page.
- Toast "Pulled qa_<MMDD>_pulled"; the drawer closes.
- **Data → Datasets** lists `qa_<MMDD>_pulled` with at most 50 rows.

#### CONN-012 · Load with a query, and its validation · P1
As: owner

1. On the connection's page, press **Load with a query**.
2. Press **Pull dataset** with everything empty.
3. Fill **Query** `SELECT 1 AS one`, **Dataset name** `1bad`, and press **Pull dataset**.
4. Set **Dataset name** `qa_<MMDD>_one`, **At most this many rows** `0`, and press **Pull dataset**.
5. Clear the row limit and press **Pull dataset**.

Expect:
- Steps 2 and 3: the browser stops the form (Query required; name pattern
  `[A-Za-z_][A-Za-z0-9_]{0,62}`).
- Step 4: "The row limit must be a whole number." in the drawer.
- Step 5: toast "Pulled qa_<MMDD>_one".
- A table named after an SQL keyword (for example `order`) is offered as `order_data`.

#### CONN-013 · Delete a connection, wrong name then right · P1
As: owner · Needs: a connection made by this run

1. On the list, press **Delete** on `qa-<MMDD>-pgbad` (or another run connection).
2. Type `qa-wrong`.
3. Clear it and type the exact name, then press **Delete**.

Expect:
- Dialog "Delete qa-<MMDD>-pgbad?" with "This removes the connection qa-<MMDD>-pgbad and its sealed
  credential from the workspace. Datasets already loaded from it stay." and "Type
  `qa-<MMDD>-pgbad` to confirm".
- Step 2: **Delete** stays disabled.
- Step 3: toast "Deleted qa-<MMDD>-pgbad"; the row is gone; datasets pulled from it remain.
- From the connection's own page, the same delete returns to the list.

#### CONN-014 · An unknown connection · P2
As: owner

1. Open `/connections/connection?name=qa-nope`, then `/connections/connection`.

Expect:
- "No connection named qa-nope", then "No connection chosen", each with "It may have been deleted.
  Connections lists the ones this workspace has." and **All connections**.

#### CONN-015 · Old Datasets links move here · P2
As: owner

1. Open `/train/data?browse=qa-<MMDD>-pg`.
2. Open `/train/data#connections`.

Expect:
- Step 1 lands on the connection's page; step 2 on Data → Connections. Back does not return to the
  old address (it was replaced).

#### CONN-016 · A reader sees connections but cannot change them · P1
As: member

1. On `W1`, open **Data → Connections**, press **Add connection**, fill a connection and save.
2. Press **Delete** on a row, type its name and confirm.
3. Open a connection and pull a table.

Expect:
- The list, figures and tables are visible.
- Each write is refused with Lake's message in its drawer or dialog; nothing is created or deleted.

#### CONN-017 · Connections at phone width · P2
As: owner

1. At 390×844, open the list, then a connection, then **Load into a dataset**.

Expect:
- The figures stack; the table scrolls inside its box.
- On the connection's page the side card is below the tables.
- The drawer fills the screen and **Pull dataset** stays reachable with the keyboard up.

## Transcripts

#### TRSC-001 · The empty Transcripts page · P1
As: owner · Needs: a workspace with no transcript (`W2`)

1. Open **Data → Transcripts** on `W2`.

Expect:
- Title "Transcripts", sub "Conversations, voice memos and calls as text you can search, edit and ask
  Orama about."
- Figures "Transcripts" `0` "in this workspace", "Recorded" "—" "of audio, in this list", "Latest"
  "—" "nothing yet".
- "No transcripts yet" with "Record a conversation, upload a voice memo or a text file, or paste a
  transcript. On a phone, a voice memo saved to Files uploads the same way." and **New transcript**.

#### TRSC-002 · Paste a transcript · P0
As: owner

1. On `W1`, press **New transcript**.
2. Check the four choices, then press **Paste**.
3. Type two lines into **Text**, the first `qa-<MMDD>-pasted standup notes`.
4. Press **Save transcript**.

Expect:
- Step 2: dialog "New Transcript", subtitle "Record a conversation, upload a voice memo or a text
  file, or paste a transcript.", choices **Record**, **Upload audio**, **Upload text**, **Paste**.
- Step 3: the dialog title is "Paste a Transcript"; **Title** fills itself from the first line.
- Step 4: toast "Transcript saved"; the transcript's page opens with eyebrow "Transcript · Pasted".

#### TRSC-003 · Save stays off until it can save · P2
As: owner

1. **New transcript → Paste**. Leave **Text** empty.
2. Type text, then clear **Title**.

Expect:
- In both steps **Save transcript** is disabled.
- **Back** returns to the four choices and clears what was typed.

#### TRSC-004 · Upload a text file · P1
As: owner · Needs: TD-AUDIO (`.vtt`)

1. **New transcript → Upload text**. Choose the `.vtt` file.
2. Read the preview, then press **Save transcript**.

Expect:
- The field "Text file" has help "WebVTT and SubRip captions lose their timings."
- **Title** is the file name without its extension, `_` as spaces.
- A "Preview" shows the file's text (cut after 4,000 characters with "…").
- After saving, the page's eyebrow is "Transcript · Uploaded" and the text has no caption timings.

#### TRSC-005 · Files Lake refuses before upload · P1
As: owner · Needs: a `.pdf`, `empty.txt`, a text file over 5 MB

1. **Upload text**, choose the `.pdf`.
2. Choose `empty.txt`.
3. Choose the file over 5 MB.
4. **Back**, **Upload audio**, choose `empty.txt`.

Expect:
- Step 1: "That isn't a text file Lake takes. Choose TXT, Markdown, WebVTT (.vtt) or SubRip (.srt)."
- Step 2: "That file is empty."
- Step 3: "That file is over the 5 MB limit."
- Step 4: "That isn't a recording Lake takes. Choose M4A or MP4, MP3, WAV, FLAC, WebM or Ogg."
- No request to `create_transcript` is made.

#### TRSC-006 · Upload audio · P1
As: owner · Needs: TD-AUDIO (`.m4a` or `.mp3`); a Lake with speech-to-text

1. **New transcript → Upload audio**, choose the recording.
2. Press **Transcribe and save** and keep the dialog open.

Expect:
- The field "Recording" has help "On a phone: Choose File, then Files (where a saved voice memo is)
  or Browse." and a **From your phone** link.
- While it works: "Uploading and transcribing · <elapsed>. A long recording takes a minute or more;
  keep this open."
- Then toast "Transcript saved"; the page shows eyebrow "Transcript · Uploaded" and the spoken words.

#### TRSC-007 · Record from the microphone · P1
As: owner · Needs: a microphone

1. **New transcript → Record**. Press the red **Start recording** button and speak for 10 seconds.
2. Press **Stop recording**, play the preview, then press **Transcribe and save**.

Expect:
- Before recording: "Tap to start. Up to 60 minutes or 25 MB; Cancel discards it, Stop keeps it to
  transcribe."
- After stopping: "Recorded" with the length (`0:10` or similar), **Record again**, and an audio
  player. **Title** is "Recording, <Mon d>, <HH:MM>".
- The saved transcript's eyebrow is "Transcript · Recorded", its text card has a "Transcribed" chip,
  and its "Length" figure shows the length.

#### TRSC-008 · Dictate into a pasted transcript · P2
As: owner · Needs: a microphone

1. **New transcript → Paste**, type a word, press the microphone (**Dictate: speak and it is added to
   the text**), speak a sentence, and stop.

Expect:
- "Transcribing…" shows, then the spoken text is added after the typed word.

#### TRSC-009 · A Lake without speech-to-text · P1
As: owner · Needs: a Lake with no speech-to-text

1. Open **Data → Transcripts**, then **New transcript**.
2. Press **Paste** and save a short text.

Expect:
- The list shows the banner "Speech-to-text is not configured on this Lake." with "Recording and audio
  uploads are off for now. Upload a text file or paste a transcript instead."
- In the dialog the same banner shows ("Recordings and audio files can't be transcribed here. Upload a
  text file or paste a transcript instead."); **Record** and **Upload audio** are disabled; **Upload
  text** and **Paste** work.
- If the first audio save is what discovers it, Lake answers `not_supported`, the audio choices turn
  off for the tab, and no error is left in the dialog.

#### TRSC-010 · The list, search and paging · P1
As: owner · Needs: the transcripts above

1. Open **Data → Transcripts**.
2. Type a word that appears in one transcript's text in **Search transcripts**.
3. Type `qa-zzz`, then press **Clear the search**.

Expect:
- Cards are newest first: title, date, an excerpt, a source chip ("Recorded", "Uploaded" or
  "Pasted"), the length for audio, and "by you".
- Step 2: after a short pause the address gains `?q=<word>`, the cards narrow, the excerpt shows the
  text around the match, and the "Transcripts" figure reads `matching “<word>”`.
- Step 3: "No matches" with "No transcript mentions “qa-zzz”."; the button clears the box and `?q=`.
- With more than 50 transcripts, **Show more** loads the next ones and "Transcripts" counts them all.

#### TRSC-011 · Open New transcript from the address · P2
As: owner

1. Open `/transcripts?new=1`.

Expect:
- The New Transcript dialog is open on its four choices.

#### TRSC-012 · A transcript's page · P1
As: owner · Needs: the pasted transcript

1. Open the pasted transcript.
2. Press **Copy** and paste somewhere.

Expect:
- Address `/w/<W1>/transcript?id=<id>`; breadcrumb Transcripts › <title>.
- Sub "Added <date and time> by you".
- Figures "Words", "Length" "—" with "no recording", "Source" "Pasted", "Recorded" (a date).
- Header buttons **Ask Orama about this**, **Copy**, **Edit**, **Delete**.
- Step 2: the button reads "Copied" for a moment; the pasted text is the transcript's text.

#### TRSC-013 · Ask Orama about a transcript · P1
As: owner

1. On the transcript's page, press **Ask Orama about this**.
2. Ask a question about the text and send it.

Expect:
- The Ask Orama panel opens with a context chip "Transcript: <title>" and the box starting "About the
  transcript “<title>”: ".
- The answer draws on the transcript's text. After that message the chip is gone.

#### TRSC-014 · Edit a transcript · P1
As: owner

1. Press **Edit**.
2. Without changing anything, look at **Save**.
3. Change the title to `qa-<MMDD>-edited` and press **Save**.
4. Edit again, clear the title, and look at **Save**.

Expect:
- Dialog "Edit Transcript" with **Title**, **Text** and a microphone.
- Step 2: **Save** is disabled.
- Step 3: toast "Transcript saved"; the page title is `qa-<MMDD>-edited`. The request sends only
  `title` (Network panel).
- Step 4: **Save** is disabled.

#### TRSC-015 · Delete a transcript · P1
As: owner · Needs: a transcript made by this run

1. Press **Delete**, then **Cancel**.
2. Press **Delete**, then **Delete transcript**.

Expect:
- Dialog "Delete “<title>”?" with "The transcript “<title>” and its text are removed from this
  workspace. This can't be undone."
- Step 1 leaves it in place.
- Step 2: toast "Transcript deleted"; the list opens without it.

#### TRSC-016 · An unknown transcript · P2
As: owner

1. Open `/transcript`, then `/transcript?id=qa-nope`.

Expect:
- The first shows an error block "No transcript was named. Open one from Transcripts."
- The second shows an error block with Lake's `not_found` message.

#### TRSC-017 · A reader cannot add or change transcripts · P1
As: member

1. On `W1`, open **Data → Transcripts** and a transcript.
2. Try **New transcript → Paste → Save transcript**, then **Edit → Save**, then **Delete**.

Expect:
- The list and text are visible.
- Each write shows Lake's `unauthorized` message in its dialog; nothing changes.

#### TRSC-018 · Transcripts on a phone · P1
As: owner

1. At 390×844, open **Data → Transcripts**, then **New transcript → Paste**.
2. Focus **Text** so the keyboard opens, then save.
3. Open a transcript and **Edit** it.

Expect:
- Cards are one column; the search box is full width.
- The dialog is a full-screen sheet; **Back** and **Save transcript** stay above the keyboard.
- **From your phone** opens a panel that says how to save a voice memo to Files on this phone.
- **Edit Transcript** is a full-screen sheet with **Cancel** and **Save** above the keyboard.

## MCP Connections

#### MCPC-001 · The empty MCP Connections page · P1
As: owner · Needs: `W2` with no MCP connection

1. Open **Data → MCP Connections** on `W2`.

Expect:
- Title "MCP Connections", sub "Other MCP servers this workspace uses. Lake is their client: it holds
  the credential, and people, Ask Orama and agents use the tools you allow."
- Figures "Connections" `0`, "Tools Allowed" `0` "across every connection", "Calls This Month" `0`
  "to these servers, metered".
- "No MCP connections yet" with "Connect a remote MCP server by its URL. Lake discovers its tools, and
  you choose which this workspace may use and which count as changes a person confirms." and **New
  connection**.

#### MCPC-002 · The New MCP Connection form · P2
As: owner

1. Press **New connection**.
2. Type `Ticket Desk 2` in **Name**, then `9 lives`.
3. Choose each **Authentication** option in turn.

Expect:
- Dialog "New MCP Connection", subtitle "An MCP server this workspace will use."; **Where the server
  runs** offers "At a URL" (chosen) and "Here, from a package (stdio)".
- **Server URL** help "Streamable HTTP or HTTP+SSE, over https. Lake connects only to public
  addresses."; **Transport** "Automatic", "Streamable HTTP", "HTTP+SSE (older servers)".
- Step 2: the Name help reads "Its tools are named `ticket_desk_2__<tool>` outside the connection.",
  then `lives__<tool>` (a leading digit is dropped).
- **Add connection** is disabled until **Name** and **Server URL** are filled.
- "Bearer token from a secret" adds **Secret mount**, **Secret**, **Field** (`token` filled in);
  "Sign in (OAuth)" shows the PKCE sentence and disables **Test**.

#### MCPC-003 · Test a server, then add it · P0
As: owner · Needs: TD-MCP (no auth)

1. **New connection**, **Server URL** = the no-auth server, press **Test**.
2. Fill **Name** `qa-<MMDD>-everything` and press **Add connection**.

Expect:
- Step 1: banner "Reached <server name>" with "<n> tools over Streamable HTTP, in <ms> ms. Nothing was
  kept." (or HTTP+SSE). The list is unchanged.
- Step 2: toast "Connected to qa-<MMDD>-everything"; the connection's page opens with status
  "Connected" and the tools listed.

#### MCPC-004 · A server Lake cannot use · P1
As: owner

1. **New connection**, **Server URL** `https://example.com/`, press **Test**.
2. Try `http://10.0.0.5/mcp` and press **Test**.

Expect:
- Step 1: banner "Lake could not use the server" with the reason.
- Step 2: refused (not https, or not a public address) with Lake's message under **Server URL** or in
  the banner.

#### MCPC-005 · Add a server with a bearer token · P1
As: owner · Needs: TD-MCP (bearer), TD-SECRET holding its token

1. **New connection**, URL of the bearer server, **Bearer token from a secret**.
2. Choose the mount, **Secret** = the secret holding the token, **Field** = its field.
3. Fill **Name** `qa-<MMDD>-bearer`, press **Test**, then **Add connection**.

Expect:
- Text "The token stays in a secret of this workspace. Lake reads it for each call and never shows it
  to anyone, a model or an agent."
- **Test** reaches the server. The new connection's "Authentication" figure is "Bearer token" with
  "secret <name>". No token appears on any page or in the Network panel.

#### MCPC-006 · Add a package server (stdio) · P1
As: owner · Needs: TD-MCP (package), TD-SECRET

1. **New connection**, choose "Here, from a package (stdio)".
2. Keep **Runtime** "npm package (npx)", **Package** `@modelcontextprotocol/server-everything@<version>`,
   choose **Secret mount**.
3. **Add variable**: **Name** `MCP_X`; then `1ABC`; then `QA_MODE` with **From** "A value" `test`.
4. Fill **Name** `qa-<MMDD>-stdio` and press **Add connection**.

Expect:
- The text "Lake runs the package as an App of this workspace, metered like any App, and reaches it
  inside the cluster. The workspace needs internet access for the package to install." No **Test**
  button.
- Step 3: "MCP_, APP_ and LAKE_ are Lake's", then "Letters, digits and _, not starting with a digit";
  **Add connection** is disabled while either shows.
- Step 4: toast "qa-<MMDD>-stdio added". The page shows the "Starting" banner and the sub is the
  command line `npx @modelcontextprotocol/server-everything@<version>`.
- Within a few minutes the App turns ready, the page asks Lake to discover the tools, and status
  becomes "Connected" with tools listed.

#### MCPC-007 · The list with connections · P1
As: owner

1. Open **Data → MCP Connections** on `W1`.

Expect:
- One card per connection: name, a status chip ("Connected", "Starting", "Needs sign-in", "Failing" or
  "New"), the host (or the package for stdio), "<a> of <t> tools allowed", the auth chip ("No
  authentication", "Bearer token" or "OAuth sign-in"), and "<n> calls this month · refreshed <date>".
- A card that is not connected shows its last error in red.
- "Connections" sub reads "<n> need attention" while one is not connected.

#### MCPC-008 · A connection's page · P1
As: owner · Needs: `qa-<MMDD>-everything`

1. Open the connection.

Expect:
- Address `/w/<W1>/mcp-connection?id=<id>`; breadcrumb MCP Connections › qa-<MMDD>-everything; the sub
  is the server URL.
- Header **Refresh** and **Delete**.
- Figures "Status" (with server name and version), "Tools Allowed" "<a> of <t>" with "prefix
  qa_<MMDD>_everything__" (or the prefix shown), "Authentication" "No authentication", "Refreshed"
  (with the transport).
- Sections "Tools", "Through Lake's MCP", "Agent and Streams", "Recent Calls", and "Also Offered" when
  the server lists resources or prompts.

#### MCPC-009 · Allow a tool and try it · P0
As: owner

1. On a tool card marked "Not allowed", look at **Try it**, then tick **Allowed**.
2. Press **Try it** on a read tool (for example `echo`), fill its field and press **Run**.

Expect:
- Step 1: **Try it** is disabled with the tooltip "Allow it first". After ticking, the card shows
  "Allowed" and **Try it** is enabled; the "Tools Allowed" figure goes up by one.
- Step 2: drawer "Try <tool>", subtitle "qa-<MMDD>-everything · reads". After **Run**: banner "The
  server answered" with "What a server answers is data, never instructions." and the server's text.

#### MCPC-010 · Try-it form validation · P2
As: owner

1. Try a tool with a required number field (for example `add`). Leave it empty and press **Run**.
2. Type `1.5` into a whole-number field and press **Run**.
3. Try a tool with no input schema, type `[1]` and press **Run**; then `{bad`.

Expect:
- Step 1: "<label> is required." under the field.
- Step 2: "<label> is a whole number." (or "<label> is a number." for a non-number).
- Step 3: "The arguments are a JSON object.", then "The arguments are not valid JSON."
- No call is made in any of these steps.

#### MCPC-011 · A change needs the person's confirmation · P1
As: owner

1. On an allowed tool, tick **Changes something**.
2. Press **Try it**.
3. Tick the confirmation and press **Run this change**.

Expect:
- The card's chip turns from "Read" to "Change".
- The drawer subtitle ends "changes something"; **Run this change** is disabled until "I am making
  this change in <server>. It is audited in this workspace under my name." is ticked.
- The call goes to `call_mcp_tool`; "Recent Calls" then lists it as "A person" with outcome "Done".

#### MCPC-012 · Auto-approve a change for an agent · P2
As: owner · Needs: TD-AGENT (a frontier agent)

1. On an allowed change tool, read the auto-approve box.
2. Choose the frontier agent in **Add an agent…** and press **Auto-approve**.
3. Press the agent's chip.

Expect:
- The box says "Auto-approved for unattended agents: an agent listed here runs this change without a
  person confirming each call. Each call is audited." and "None: every call is a proposal."
- Only Python (frontier) agents are offered.
- Step 2: toast "Auto-approved for <agent>"; a chip "<agent> ✕" appears.
- Step 3: toast "No longer auto-approved for <agent>"; the chip goes.

#### MCPC-013 · Expose through Lake's MCP · P1
As: owner

1. In "Through Lake's MCP", press **Turn on**, read the dialog, and press **Cancel**.
2. Press **Turn on** again and confirm **Turn on**.
3. Press **Turn off**.

Expect:
- The section starts "Off. Only this workspace's people, Ask Orama and its agents use these tools,
  through Lake."
- The dialog "Expose these tools through Lake's MCP?" says "Anyone who may call Lake's tools in this
  workspace will be able to call these tools, and Lake calls <server> for them with this connection's
  credential. Lake's permission on the workspace is the only gate."
- Step 2: toast "Exposed through Lake's MCP"; the section reads "On." with the same warning; the list
  card shows "Through Lake's MCP".
- Step 3: toast "No longer exposed through Lake's MCP" (no dialog).

#### MCPC-014 · An agent and streams for a connection · P2
As: owner

1. In "Agent and Streams", look at the inputs, then type a prompt in **What its agent is for** and
   press **Create an agent for this connection**.
2. Type a topic in **Input stream** and **Notifications stream** and press **Save streams**.

Expect:
- Before the agent exists, **Input stream** and **Output stream** are disabled and **Create an agent
  for this connection** is disabled while the prompt is empty.
- Step 1: toast "Agent created"; the section reads "This connection has an agent. Its turns act with
  the authority of the person who set its streams." with **Open the agent**. The list card shows "Has
  an agent".
- The stream fields suggest the workspace's topics. Step 2: toast "Streams saved".

#### MCPC-015 · Refresh a connection · P2
As: owner

1. Press **Refresh** in the header.

Expect:
- Toast "<n> tools from qa-<MMDD>-everything"; "Refreshed" shows the new date.

#### MCPC-016 · OAuth sign-in · P1
As: owner · Needs: TD-MCP (OAuth), TD-SECRET

1. Add `qa-<MMDD>-oauth` with **Sign in (OAuth)**.
2. On its page read the banner, then press **Sign in**.
3. Look at **Continue to sign in**, choose the mount, press it, and sign in at the server.

Expect:
- Step 2: banner "A person needs to sign in" with a **Sign in** button.
- Step 3: dialog "Sign In to <server>" says the refresh token is kept "as the secret `mcp-<prefix>` in
  the mount you choose"; **Continue to sign in** is disabled until a mount is chosen. The browser goes
  to the server, then to "Signing In" ("Finishing the sign-in and asking the server what it offers…"),
  then back to the connection's page with status "Connected" and the header button **Sign in again**.

#### MCPC-017 · The sign-in callback's failures · P2
As: owner

1. Open `/mcp-connections/oauth/callback`.
2. Open `/mcp-connections/oauth/callback?error=access_denied&error_description=User%20cancelled`.

Expect:
- Step 1: banner "The sign-in did not finish" with "This page was opened without a sign-in to finish.
  Start again from the connection's page." and **Back to MCP Connections**.
- Step 2: "The server did not sign you in: User cancelled."

#### MCPC-018 · Sign in with no secret mount · P2
As: owner · Needs: `W2` with no secret mount and an OAuth connection added there

1. On the OAuth connection in `W2`, press **Sign in**.

Expect:
- "This workspace has no secret mount yet. Make one in Secrets first." and **Continue to sign in**
  stays disabled.

#### MCPC-019 · A package server runs as an App · P1
As: owner · Needs: `qa-<MMDD>-stdio`

1. Open `qa-<MMDD>-stdio` and read "Runs in This Workspace".
2. Press **Stop**, wait, then **Start**; then **Restart**.
3. Press **Refresh** on the logs.
4. Press the App link under "Details".

Expect:
- Rows "App" (a phase chip), "Package", "Runtime" "npm package", "Command", "Environment" (`QA_MODE`;
  a secret variable reads `<NAME> (secret)`), "Details".
- Step 2: toasts "Stop requested", "Start requested", "Restart requested"; only **Start** shows while
  stopped.
- The terminal "Logs · last 200 lines" shows what the package printed.
- Step 4 opens the App's page in Ship → Apps (see Suspected defects 1).

#### MCPC-020 · Delete a connection, wrong name then right · P1
As: owner · Needs: `qa-<MMDD>-stdio`

1. Press **Delete**, type `qa-wrong`.
2. Type `qa-<MMDD>-stdio` and press **Delete connection**.

Expect:
- Dialog "Delete qa-<MMDD>-stdio?" with "People, Ask Orama and agents lose its tools at once, and its
  agent loses its only tools. Its App is deleted with it. The secret it used stays in its mount."
- Step 1: **Delete connection** is disabled.
- Step 2: toast "qa-<MMDD>-stdio deleted"; the list opens without it; its App is gone from Ship →
  Apps.

#### MCPC-021 · An unknown connection · P2
As: owner

1. Open `/mcp-connection`, then `/mcp-connection?id=qa-nope`.

Expect:
- "No connection was named. Open one from MCP Connections.", then an error block with Lake's
  message and **Retry** unless it is a 4xx.

#### MCPC-022 · A reader cannot change MCP connections · P1
As: member

1. On `W1`, open **Data → MCP Connections** and a connection.
2. Try **New connection → Add connection**, tick **Allowed** on a tool, **Turn on**, and **Delete**.

Expect:
- The list and pages are visible.
- Each write is refused with Lake's message (a toast for the tool switch, the dialog for the
  others); nothing changes after a reload.

#### MCPC-023 · MCP Connections on a phone · P2
As: owner

1. At 390×844, open the list, press **New connection**, and focus **Server URL**.
2. Open a connection and **Try it** on a tool.

Expect:
- New MCP Connection is a full-screen sheet; **Cancel**, **Test** and **Add connection** stay above the
  keyboard.
- The tool cards, switches and **Try it** fit the width; the Try drawer fills the screen.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/mcp/McpConnectionPage.tsx:565` | The stdio App link is built as `wsPath("/ship/app") + qs(…)`; that `qs` has no leading `?`, so the link is `/ship/appcustomer_id=…` and opens Not Found. It is also a plain `<a>` (a full reload) | MCPC-019 |
| 2 | `frontend/src/pages/connections/ConnectionPage.tsx:38` | A failed `list_connections` read shows "No connection named <name>" / "It may have been deleted." instead of the error | CONN-014 (block the `workspace_query` request) |
| 3 | `frontend/src/pages/mcp/NewMcpConnection.tsx:89` | **Test** with a bearer token is disabled until **Name** is filled; with no auth it works without a name | MCPC-005 |
| 4 | `frontend/src/pages/mcp/McpConnectionsPage.tsx:30` | **New connection** shows and opens while the workspace gate blocks the page | MCPC-001 on a not-ready workspace |
| 5 | `frontend/src/pages/transcripts/TranscriptsPage.tsx:69` | **New transcript** shows and opens while the workspace gate blocks the page | TRSC-001 on a not-ready workspace |
| 6 | `frontend/src/pages/mcp/McpConnectionsPage.tsx:44` | With no connections, "Connections" reads "all connected" | MCPC-001 |
| 7 | `frontend/src/pages/mcp/McpConnectionPage.tsx:309` | **Create an agent for this connection** and **Save streams** share one busy flag and one error, so both spin together | MCPC-014 |
| 8 | `frontend/src/pages/transcripts/NewTranscript.tsx:243`, `TranscriptPage.tsx:176` | Title is capped at 200 characters; the guide (`docs/guide/transcripts.md:179`) says 1 to 300 bytes | TRSC-014 |
| 9 | `frontend/src/pages/transcripts/TranscriptPage.tsx:105` | Delete transcript needs no typed confirmation, unlike connections and MCP connections | TRSC-015 |
| 10 | `frontend/src/pages/connections/forms.tsx:134` | Add a Connection has no way to test the credential; a wrong password shows only as "did not answer" | CONN-007 |
| 11 | `docs/ui/transcripts.md:51` | Quotes "…not configured on this lake." while the app says "…on this Lake." | TRSC-009 |
