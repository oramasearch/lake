# Developer

The Developer group: **API** (`/developer/api`: keys, a quick start and every operation from
`/v1/openapi.json`), **MCP** (`/developer/mcp`: six steps to connect an agent) and **Docs**
(`/docs`: Lake's guides, open to visitors too). A key made on either page is an API token; the
account's token page is in [19-account-settings.md](19-account-settings.md).

## Before you start

- Personas: `owner`, `operator`, `visitor`.
- Data: W1.
- This file makes three keys and revokes them again: `qa-<MMDD>-api-read` (DEV-005),
  `qa-<MMDD>-api-named` (DEV-007), `qa-<MMDD>-mcp` (DEV-019). Never paste a key's value into a
  results file or a screenshot.
- A terminal with `curl` for DEV-006.
- Code block titles are drawn in capitals by the stylesheet; the quoted titles below are the text as
  written.

## API

#### DEV-001 · Open Developer → API · P0
As: owner

1. Press **Developer → API** in the rail.

Expect:
- Breadcrumb Developer › API. Eyebrow "Developer", title "API", sub "Call Lake from a program as
  you, with exactly your permissions. Every page on this site, Ask Orama and the MCP server use this
  same API: one registry, REST under /v1, OpenAPI and MCP."
- Actions **openapi.json**, **API guide** and **Set up MCP** (primary).
- A strip "Base URL" with `<origin>/v1`, a **Copy** button and chips "REST · JSON", "OpenAPI 3.1",
  "{ok, value, duration_ms}", "Authorization: Bearer <key>".
- Tabs "Keys" (current), "Quick start", "Operations".
- On Keys: a card "Create an API Key" with the form, a card "Your Keys", the line "A key works for
  both the REST API and MCP. Sign-in sessions and devices are on Sessions; every token is also on
  Tokens." and a card "The Rules a Key Lives By" with "Your permissions, nothing more", "Read-only
  keys", "Expiry and revocation", "Every call is audited", "One envelope", "No secrets come back".

#### DEV-002 · The header actions · P1
As: owner

1. Press **Copy** in the Base URL strip.
2. Press **openapi.json**.
3. Press **API guide**, go back, press **Set up MCP**.

Expect:
- Step 1: the button reads "Copied" for a moment; the clipboard holds `<origin>/v1`.
- Step 2: the button spins, then the browser saves `lake-openapi.json`, pretty-printed JSON.
- Step 3: **API guide** opens `/docs/developer`; **Set up MCP** opens `/developer/mcp`.

#### DEV-003 · The key form · P1
As: owner

1. Read the form on the Keys tab.
2. Choose **Named capabilities** and press **Create an API key** with the capabilities box empty.
3. Choose **Every operation**, set **Lifetime** to "A number of seconds", type `0` in **Lifetime in
   seconds**, press **Create an API key**.
4. Set **Lifetime** to "Never (a standing key)", leave **Why this key exists** empty, press
   **Create an API key**.

Expect:
- Step 1: **Name** (placeholder "Nightly export", help "Where it will be used: you will see it in
  the list below."), **Lifetime** with "As long as my sign-in allows (at most 30 days)", "1 day",
  "7 days", "A number of seconds", "Never (a standing key)", and help "A key never outlives the
  sign-in that made it, unless it is a standing key your space allows."; **Access** cards "Every
  operation", "Read-only", "Named capabilities", each with its help line; the checkbox "Static
  exception: a key that is not tied to a sign-in session (needs a reason)"; the button and "The key
  is shown once. Lake keeps only its hash."
- Step 2: a red notice "Name at least one capability the key may call."; nothing is sent.
- Step 3: "Give the lifetime in seconds, 1 or more."
- Step 4: **Why this key exists** appears (placeholder "the nightly sync from our data warehouse",
  help "Kept with the key. A standing key also needs your space's opt-in."); pressing gives "Say why
  this key exists: a standing or static key keeps its reason." With **Every operation** and Never,
  the note "A key that never expires names what it may call, so this one names every capability
  Lake has today." shows.

#### DEV-004 · A standing key without the opt-in · P2
As: owner

1. Set **Lifetime** to Never, **Access** to Read-only, fill **Why this key exists**, and press
   **Create an API key**.

Expect:
- When the owner's space has not opted in to standing keys, a red notice with Lake's refusal;
  nothing is listed in Your Keys.
- When it has, the key is made (revoke it at once on Your Keys).

#### DEV-005 · Create a read-only API key · P0
As: owner

1. Type `qa-<MMDD>-api-read` in **Name**, choose **Read-only**, keep the default lifetime, and press
   **Create an API key**.

Expect:
- The form is replaced by a green box "Your New Key: Copy It Now", "This is the only time Lake shows
  it. Keep it in a password manager or your client's secret settings. It is filled into Quick start
  and every operation's curl until you leave this page.", the key with **Copy**, and **I've stored
  it**.
- "Your Keys" lists `qa-<MMDD>-api-read`: "api token", Access "Read-only", Expires in about 30
  days or less, Last used "never", Created today, State "Active".

#### DEV-006 · Quick start with the new key · P1
As: owner · Needs: DEV-005 (the green box still open)

1. Open the **Quick start** tab.
2. Copy the "who am I" block and run it in a terminal.
3. Go back to **Keys**, press **I've stored it**, and open **Quick start** again.

Expect:
- Step 1: no "Use Your Own Key" notice; every block holds the key in place of `$LAKE_KEY`.
- Step 2: `curl` prints `{"ok":true,…}` naming the owner.
- Step 3: the blocks read `$LAKE_KEY` again and the notice is back (see Suspected defects 1).

#### DEV-007 · A key for named capabilities · P2
As: owner

1. Name it `qa-<MMDD>-api-named`, choose **Named capabilities**, press the
   `refs,find_missing,push,pull,upload_chunks,read_chunk` link in the help, and create it.
2. Press **I've stored it**.

Expect:
- Step 1: the link fills **Capabilities, comma separated** with those six names.
- Your Keys lists it with Access "6 capabilities", the names in its tooltip.

#### DEV-008 · Quick start without a key · P1
As: owner (reload the page so no key is held)

1. Open **Quick start**.

Expect:
- A notice "Use Your Own Key" explaining `LAKE_KEY` (`$LAKE_KEY`, `process.env.LAKE_KEY`,
  `os.environ["LAKE_KEY"]`).
- Blocks "who am I", "a list read: GET, input in the query", "an action: POST, input as JSON",
  "from JavaScript", "from Python: send a User-Agent", "in a workspace: /w/<id>/v1" and "the whole
  contract", each with **Copy**. The action and workspace blocks hold the current account and W1
  ids.
- Cards "The Envelope", "Reads and Actions" and "Workspaces".

#### DEV-009 · Your Keys · P1
As: owner · Needs: DEV-005, DEV-007

1. Read the table, then tick **Show revoked and expired**.
2. Press the "Key", "Expires", "Last used" and "Created" headers.

Expect:
- Step 1: columns "Key", "Access", "Expires", "Last used", "Created", "State" and Revoke. Newest
  first by default. Ticking adds revoked and expired keys, with State "Revoked" or "Expired" and no
  Revoke on revoked rows.
- Step 2: each sorts by its column.
- A key that never expires reads "Never" with a warning icon and its reason on hover.

#### DEV-010 · Revoke a key · P1
As: owner · Needs: DEV-007

1. Press **Revoke** on `qa-<MMDD>-api-named`.
2. Press **Cancel**.
3. Press **Revoke** again, then **Revoke** in the dialog.

Expect:
- Step 1: a dialog "Revoke the key?" with "**qa-<MMDD>-api-named** stops working at once, and so
  does anything that uses it."
- Step 2: nothing changes.
- Step 3: the toast "Key revoked: it stops working now"; the row leaves the table (it shows again
  with **Show revoked and expired**, State "Revoked").

#### DEV-011 · Operations · P1
As: owner

1. Open the **Operations** tab.
2. Type `workspace` in **Find an operation: path, name or group**.
3. Press the "write" chip, then "All <n>".
4. Press an operation row, then press it again.

Expect:
- Step 1: a skeleton, then a line "<n> operations · <n> groups · OpenAPI <version>" and one section per
  group with its count; each row a method badge (GET green, POST blue), the path and its summary.
- Step 2: only matches show; the line reads "<shown> of <total> operations …".
- Step 3: "write" shows only writes; "All" shows everything again.
- Step 4: the row opens (`aria-expanded`): the description, chips (its operationId, "read" or
  "write: refused to a read-only key", "needs <permission>", "no key needed" for public ones),
  "Parameters", "Request body · …", "Response · the value inside {ok, value, duration_ms}", and a
  block "copy as curl" with `$LAKE_KEY` (or the fresh key). Pressing again closes it.

#### DEV-012 · Operations: no match and failure · P2
As: owner

1. Type `zzqqxx` in the operations search.
2. Block `/v1/openapi.json` in the Network panel and reload `?tab=operations`.

Expect:
- Step 1: "No operation matches “zzqqxx”."
- Step 2: "Couldn't load the OpenAPI document" with the message "The OpenAPI document did not load
  (<status>)." (or the network message) and **Retry**.

#### DEV-013 · Tabs in the address · P2
As: owner

1. Open `/developer/api?tab=operations`, then `?tab=start`, then `?tab=nosuch`.
2. Press the **Keys** tab.

Expect:
- Each opens its tab; an unknown tab opens Keys.
- Step 2: the address has no `tab`. Switching tabs replaces the history entry.

#### DEV-014 · API for the operator · P1
As: operator

1. Open **Developer → API**.

Expect:
- The sub reads "Call Lake from a program as the operator, with the admin token. …".
- The Keys tab shows only the notice "Your Key Is the Admin Token": "You are signed in as the
  operator. The operator's key is the instance's admin token (`LAKE_ADMIN_TOKEN`), not a token made
  here: send it as `Authorization: Bearer <admin token>`. Keys made on this page belong to a person
  and act as that person; sign in as one to make them." and the Rules card. No form, no Your Keys.

## MCP

#### DEV-015 · Open Developer → MCP · P0
As: owner

1. Press **Developer → MCP** in the rail.

Expect:
- Breadcrumb Developer › MCP. Eyebrow "Developer", title "MCP", the sub beginning "Connect Claude
  Code, Codex, Cursor, Claude Desktop or any MCP client to Lake." and the action **API keys and
  operations** (→ `/developer/api`).
- A strip "Endpoint" with `<origin>/mcp` and **Copy**; chips "JSON-RPC 2.0", "streamable HTTP",
  "protocol 2025-06-18", "Authorization: Bearer <key>", "<n> tools".
- Six numbered steps: "Create an MCP Key", "Set Up Your Client", "Teach Your Agent", "What the
  Tools Do", "Worked Flows", "Your Keys".

#### DEV-016 · Step 1's key form for MCP · P2
As: owner

1. Read step 1.

Expect:
- The aside "Read-only is the right default for an agent that reports. Choose every tool only for
  one that should also act, still within your permissions."
- The form as on API, with **Name** placeholder "Claude Code on my laptop" and **Access** cards
  "Every tool", "Read-only tools", "Named tools"; the button reads **Create an MCP key**.
- Below, a closed "Already have a key? Paste it to fill the setup blocks. It stays in this page
  only."

#### DEV-017 · Set up a client without a key · P1
As: owner

1. Read step 2's aside and the Claude Code panel.
2. Press each client tile in turn.
3. Press **Copy path** beside a file path.

Expect:
- Step 1: "The blocks read the key from `$LAKE_KEY` until you make or paste one in step 1." The
  "Claude Code" tile is pressed; its panel shows "Claude Code", where it is saved, code blocks with
  `$LAKE_KEY`, "Check it worked." and notes.
- Step 2: tiles "Claude Code", "OpenAI Codex", "Claude Desktop", "Cursor", "VS Code", "Windsurf",
  "Gemini CLI", "Any client", "ChatGPT and Claude.ai", each with its line (e.g. "One command: HTTP,
  the key as a header"). Each press shows that client's files and blocks; only one tile is pressed.
- Step 3: the button reads "Copied" for a moment.
- Under the panel: "After Lake upgrades, reconnect or restart your client. …"

#### DEV-018 · Paste a key · P1
As: owner · Needs: DEV-005's key (copied earlier)

1. Open "Already have a key? …" and paste the key.
2. Read step 2.
3. Reload.

Expect:
- Step 1: the field is a password field named "A key you already have".
- Step 2: the aside reads "Your key is filled into every block. Pick your client and copy its
  setup." and the blocks hold the key.
- Step 3: the field is empty and the blocks read `$LAKE_KEY` again.

#### DEV-019 · Create an MCP key · P1
As: owner

1. Name it `qa-<MMDD>-mcp`, choose **Read-only tools**, and press **Create an MCP key**.
2. Press **I've stored it**.

Expect:
- Step 1: the green box "Your New Key: Copy It Now" with "… It is filled into every setup block
  below until you leave this page."; the paste box is hidden; step 2's blocks hold the key; step 6
  lists `qa-<MMDD>-mcp` with Access "Read-only".
- Step 2: the form comes back and the blocks read `$LAKE_KEY` again (see Suspected defects 1).

#### DEV-020 · Teach Your Agent · P2
As: owner

1. In step 2 choose "OpenAI Codex", then read step 3.
2. Press **Download the Lake skill**, **llms-full.txt**, **llms.txt** and **OpenAPI operations**.
3. Press each "Install for" pill.

Expect:
- Step 1: "Install for" has "Codex (AGENTS.md)" selected (it follows the client; "Any agent" for a
  client with no skill install).
- Step 2: `SKILL.md` and `lake-llms-full.txt` are saved; **llms.txt** opens `/llms.txt`; **OpenAPI
  operations** opens `/developer/api?tab=operations`.
- Step 3: "Claude Code", "Codex (AGENTS.md)", "Gemini CLI (GEMINI.md)", "Any agent" each show their
  install blocks.

#### DEV-021 · What the Tools Do · P1
As: owner

1. Read step 4.
2. Type `repository` in **Find a tool**.
3. Clear it; press the "read" chip; press a group pill; press it again.
4. Press **Show all <n>** under a long group.

Expect:
- Step 1: "<n> tools", chips "All <n>", "read <n>", "write <n>", pills "Every group" and one per
  group with its count; each group shows "<n> · read <r> · write <w>", its summary and up to six
  tools (read or write chip, name, title); the foot "Each call runs as you and is recorded in the
  audit log."
- Step 2: only matching tools; the count reads "<shown> of <total> tools".
- Step 3: "read" shows only reads; a group pill shows only that group; pressing it again shows
  every group.
- Step 4: the whole group shows. While filtering every group shows in full.

#### DEV-022 · Worked Flows · P2
As: owner

1. Read step 5.

Expect:
- One card per flow with its summary and its numbered tools, each with a note after ":".
- With none: "No worked flows are listed."

#### DEV-023 · The former tab addresses · P2
As: owner

1. Open `/developer/mcp?tab=keys`, then `?tab=tools`, then `?tab=flows`.

Expect:
- Each scrolls to step 6, 4 and 5.

#### DEV-024 · Revoke the MCP key · P1
As: owner · Needs: DEV-019

1. In step 6, press **Revoke** on `qa-<MMDD>-mcp` and confirm.
2. In the same way, revoke `qa-<MMDD>-api-read`.

Expect:
- The toast "Key revoked: it stops working now" each time; the rows leave the table.
- The keys show as "Revoked" on **Account → API Tokens** too.

#### DEV-025 · MCP for the operator · P2
As: operator

1. Open **Developer → MCP**.

Expect:
- Step 1's aside reads "The operator connects with the instance's admin token." with the notice
  "Your Key Is the Admin Token" and the paste box open.
- Step 6 reads "The operator has no keys of its own."

## Docs

#### DEV-026 · Open the Docs · P0
As: visitor, then owner

1. Press **Docs** at the foot of the rail.

Expect:
- `/docs` shows the first guide. Breadcrumb Docs › <its title>; tab title "Lake — <its title>".
- Left: "Docs", the search box "Search the docs", the sections, each a label over its pages (the
  open page current, its sections listed beneath it below 1280 px), and the links "llms.txt", "API
  reference", "MCP setup".
- The article: "Docs › <section> › <title>" with **Copy link**, the guide, and **Previous** /
  **Next** cards.
- At 1280 px or wider, "On this page" on the right.

#### DEV-027 · Move through the guides · P1
As: owner

1. Press another page in the left list.
2. Press **Next**, then **Previous**.
3. Press **Copy link**.

Expect:
- Step 1: `/docs/<slug>` opens at the top.
- Step 2: the next and previous guides open.
- Step 3: the button reads "Link copied" for a moment; the clipboard holds `<origin>/docs/<slug>`
  (or `<origin>/docs` for the first guide).

#### DEV-028 · Headings and On this page · P1
As: owner

1. Open `/docs/account#sign-in`.
2. Scroll the guide; watch "On this page".
3. Press an entry in "On this page".
4. Hover a heading and press its "#".

Expect:
- Step 1: the guide opens scrolled to "Sign In".
- Step 2: the heading in view is marked in blue.
- Step 3: the page scrolls to it and the address ends `#<anchor>` without a new history entry.
- Step 4: the address takes that heading's anchor.

#### DEV-029 · Search the docs · P1
As: owner

1. Type `t` in "Search the docs".
2. Type `token`.
3. Type a word that only appears in a guide's text.
4. Type `zzqqxx`.
5. Press **Escape**.

Expect:
- Step 1: "Search · “t”", and "Pages and sections" at once for titles and headings containing it. Typing one letter that no title or heading contains shows "Type at least two letters to search the text."; the text is not searched below two letters.
- Step 2: "Pages and sections" lists titles and headings with "token" marked; then "Searching the
  text…" and "In the text" with excerpts. A result opens its page and clears the search.
- Step 3: the page appears under "In the text" with an excerpt.
- Step 4: "Nothing Found" / "No page mentions “zzqqxx”. Try the name of a page or a button."
- Step 5: the search clears and the guide shows again; **Clear search** does the same.

#### DEV-030 · A page that does not exist · P2
As: visitor

1. Open `/docs/no-such-guide`.
2. Press **All docs**.

Expect:
- Step 1: "No Such Page" / "There is no Docs page named “no-such-guide”."
- Step 2: `/docs` opens.

#### DEV-031 · The docs cannot be read · P2
As: owner

1. Block `list_docs` in the Network panel and reload `/docs`.

Expect:
- The left list shows "Couldn't load the docs" with **Retry**.

#### DEV-032 · Docs links for a visitor · P2
As: visitor

1. On `/docs`, press **API reference**, then **MCP setup**.

Expect:
- Each sends the visitor to `/sign-in?next=…` (see Suspected defects 3).

## Phone width

#### DEV-033 · Developer pages on a phone · P2
As: owner

1. At 390 px, open **Developer → API**, then **Developer → MCP**, then `/docs`.
2. On `/docs`, press **Contents**.

Expect:
- API: the strip wraps; the Access cards stack; the tables scroll in their own boxes.
- MCP: each step's title and aside sit above its content; client tiles are one column; tool titles
  drop to their own line.
- Docs: the section list is hidden behind **Contents** (`aria-expanded`); step 2 shows it, and
  opening a page hides it again. "On this page" is a closed disclosure above the guide.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/developer/ApiPage.tsx:82`, `McpPage.tsx:127`, `parts.tsx:381` | **I've stored it** clears the key from Quick start and the setup blocks, though the box says it is filled in "until you leave this page" (also `docs/guide/developer.md:49`) | DEV-006, DEV-019 |
| 2 | `frontend/src/pages/developer/parts.tsx:243` vs `logic.ts:97,99` | The API form calls full access "Every operation"; the Your Keys table on the same page labels it "Every tool" | DEV-009 |
| 3 | `frontend/src/pages/developer/DocsPage.tsx:69-74` | "API reference" and "MCP setup" are shown to visitors, who are sent to sign in | DEV-032 |
| 4 | `frontend/src/pages/developer/McpPage.tsx:114` | Step 1 is titled "Create an MCP Key" for the operator, who cannot make one | DEV-025 |
