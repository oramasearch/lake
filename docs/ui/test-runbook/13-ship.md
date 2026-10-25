# Ship

This file covers the Ship group: **Services** (the databases, streams and engines the account runs),
**Models** and one model's serving page, **Search Indexes** and one index, **Packages**, **Apps** and
one app, **Deployments**, and the repository platform pages they link to: **Environments**, one
**Environment**, its **Logs**, and **Repository Export**. Ship → Workspaces is in
[04-workspaces.md](04-workspaces.md). Services is first because later cases use `TD-SERVICE`.

## Before you start

- Personas: `owner`, `member` (read on `W1`), `writer` (write, not admin, on `W1`), `newcomer`.
- Test data: `TD-MODEL` (two versions), `TD-DATASET`, `TD-SERVICE` (made in SHIP-003 if it does not
  exist yet), `TD-STORAGE` (a Ready volume), `TD-SECRET`, `TD-REPO`, `TD-IMAGE`, `TD-DEPLOY`.
- Write down the `server_info` flag `deployments`. When it is `false`, run only SHIP-070 of the
  Deployments, Environments, Environment and Logs cases and mark the rest `blocked`.
- This file alone also needs:
  - `TD-MODEL-METH`: a model a workspace methodology trained (kind `python:W-…`), if the Lake has
    methodologies. Otherwise mark SHIP-025 `blocked`.
  - A second dataset or none at all in `W2` (for the empty New Index drawer).
  - The Agent Library with `agent-application` deployed, for the agent cases. Otherwise mark them
    `blocked`, except SHIP-055.
  - `curl` on the tester's machine.
- Names: services `qa-<MMDD>-<kind>` (for example `qa-1022-pg`, `qa-1022-kafka`); apps
  `qa-<MMDD>-app`, `qa-<MMDD>-img`; predict and search keys `predict <model>` and `search <table>`
  (Lake's own names; revoke them in clean-up).

## Services

#### SHIP-001 · The Services page · P1
As: owner

1. Open **Ship → Services**.

Expect:
- Address `/services` (no `/w/` prefix); title "Services", eyebrow "Ship"; breadcrumb Ship ›
  Services.
- Sub "The databases, streams and engines your workspaces run on Lake, each its own, with its data
  kept apart from every other customer's."
- Figures "Services" (with "of <n> your workspaces may run at once" when the Lake sets a limit),
  "Ready", "Changing" ("nothing in progress" when nothing changes), "Failed".
- "Your Services": the workspace's own Zetta first, marked with a "workspace" chip.
- "Service Catalog" with the sub "Services run beside the workspace that Design, Train and Ship use.
  Each kind's usual start time is on its card." One card per kind with a "ready in about …" chip and
  **New**; the Zetta card is highlighted and has a "default" chip.

#### SHIP-002 · The catalog's cards · P2
As: owner

1. Read each catalog card.

Expect:
- Each known kind shows its words: Zetta "Your workspace engine", Postgres "Relational database",
  Kafka "Event streams", ClickHouse "Columnar analytics", TiDB "Distributed SQL", Cassandra
  "Wide-column store", Valkey "Key-value cache", DuckDB "Analytical SQL", Polars "DataFrame engine",
  SQLite "Embedded SQL". A kind not in that list shows a generic server icon and no words.
- Each **New** has the name "New <label> service" (screen reader or accessibility panel).

#### SHIP-003 · Create a Postgres service (TD-SERVICE) · P0
As: owner

1. Press **New** on the Postgres card.
2. Check the drawer, fill **Name** `qa-<MMDD>-pg`, keep **Instances** `1` and **Storage (GiB)** `10`.
3. Press **Create service** and stay on the page.

Expect:
- Drawer "New Postgres Service", subtitle "Usually ready in about <time>."; **Kind** is Postgres;
  **Name** help "Lowercase letters, digits and dashes, starting with a letter."
- Toast "Creating qa-<MMDD>-pg"; the row appears with a warning-tone status ("Waiting for the
  operator", then the operator's steps, then "Connecting"), and a hint "Usually ready in about <time>;
  started <time> ago."
- While it changes, the banner "This page checks again every 3 seconds while a service is changing."
  shows and "Changing" reads "checked every 3 seconds".
- The row reaches "Ready" without a reload; the banner goes. This service is `TD-SERVICE`.

#### SHIP-004 · New service form rules · P2
As: owner

1. Press **New service** in the header.
2. Type `1abc` in **Name** and press **Create service**; then `qa-<MMDD>-pg` (taken).
3. Type `0` and `101` in **Storage (GiB)**.

Expect:
- The drawer opens on Zetta ("New Zetta Service"). **Kind** lists every kind as "<label> — ready in
  about <time>"; changing it changes the title.
- **Instances** offers 1, 2, 3.
- Step 2: the browser blocks `1abc` (pattern `[a-z][a-z0-9-]{0,30}`); the taken name is refused by
  Lake under **Name** (`conflict`).
- Step 3: the browser blocks both (1 to 100).
- When the Lake limits services, "Your workspaces may run <n> services at once." shows.

#### SHIP-005 · Create a service of each kind · P1
As: owner

1. For each kind in the catalog other than Postgres, press its **New**, name it `qa-<MMDD>-<kind>`
   (for example `qa-1022-kafka`), keep the defaults, and create it.
2. For SQLite, choose **Instances** `2` first.
3. For SQLite, DuckDB and Polars, check the **Storage** field before creating.
4. Wait for every row to settle.

Expect:
- Each create toasts "Creating <name>" and its row ends "Ready" (Valkey is a preview; a "Failed"
  with the operator's message is a finding, not a page fault).
- Step 2: Lake refuses two instances for SQLite with its message in the drawer.
- Step 3: SQLite, DuckDB and Polars show a **Storage** field (help "Workspace storage to mount: a
  volume at a path (one service or app per volume), a bucket as environment variables."); other kinds
  do not.
- Each "Size" reads `<instances> × <storage> GiB`. A kind filter (with "Every kind") appears once two
  kinds exist.

#### SHIP-006 · Show a service's connection · P0
As: owner · Needs: TD-SERVICE

1. On the Ready Postgres row, press **Show connection**.
2. Copy one field, then close the dialog and reload.

Expect:
- Dialog "Connection to qa-<MMDD>-pg" with the banner "Reachable inside the Lake cluster, from your
  other services. Copy it now; it is not shown again unless you ask." and every field with a copy
  button.
- The request is a POST to `/v1/service_connection`; no password is in any address.
- After reload, nothing of the connection is on the page.
- The Zetta rows never offer **Show connection**.

#### SHIP-007 · Search and filter services · P2
As: owner

1. Type `pg` in **Search services…**, then pick the "Kafka" filter chip, then "Every kind".
2. Open `/services?q=qa-<MMDD>-pg`.

Expect:
- The table narrows by name, kind or address; a search with no match shows "No service matches" with
  "Clear the search or a filter to see more."
- Step 2 opens with the search box filled and only that row shown.

#### SHIP-008 · Mount storage into a service · P1
As: owner · Needs: TD-STORAGE (a Ready volume), the DuckDB service from SHIP-005

1. On the DuckDB row, press **Storage**.
2. Choose the volume at a path and press **Save**.

Expect:
- Dialog "Storage for qa-<MMDD>-duckdb", subtitle "Saving rolls the service's pods with the storage
  you choose.", and a link **Data → Drives → Storage**.
- Toast "Storage of qa-<MMDD>-duckdb saved"; the row reads "Mounts <volume> at <path>", the volume
  name a link to Drives → Storage.

#### SHIP-009 · Developer access from a service · P2
As: owner · Needs: TD-SERVICE

1. On the Ready Postgres row, press **Developer access**.

Expect:
- The button's tooltip is "Reach this service from your machine through a tunnel".
- Network → Developer Access opens with **Grant Developer Access** already open: the service chosen,
  its port filled in, you as **Who**, **For** "1 hour" (continue in [14-network.md](14-network.md)).

#### SHIP-010 · Delete a service, wrong name then right · P1
As: owner · Needs: the Kafka service from SHIP-005

1. On `qa-<MMDD>-kafka`, press **Delete**.
2. Type `qa-wrong` and press **Delete qa-<MMDD>-kafka**.
3. Type `qa-<MMDD>-kafka` and press it again.

Expect:
- Dialog "Delete qa-<MMDD>-kafka?", subtitle "The service and all of its data are deleted. This
  cannot be undone.", field "Type qa-<MMDD>-kafka to delete it and all of its data, which cannot be
  recovered".
- Step 2: Lake refuses; the message is under the field; the service is unchanged.
- Step 3: toast "Deleting qa-<MMDD>-kafka"; the row reads "Deleting" and has no actions, then
  disappears once the operator removes it.
- Delete every other `qa-<MMDD>-<kind>` service of SHIP-005 the same way, keeping `TD-SERVICE` and the
  DuckDB one until SHIP-008 is done.

#### SHIP-011 · Delete warns about a connection that uses it · P2
As: owner · Needs: a Postgres service that Data → Connections uses (CONN-009)

1. Press **Delete** on that service and read the dialog, then **Cancel**.

Expect:
- The warning "The connection `<name>` (Data → Connections) uses it and stops working."

#### SHIP-012 · Services by role · P1
As: member, then writer · Needs: TD-SERVICE

1. As `member`, press **New service** and create `qa-<MMDD>-m`.
2. As `writer`, create `qa-<MMDD>-w`, then press **Show connection** on TD-SERVICE, then **Delete** on
   `qa-<MMDD>-w` with its name typed.

Expect:
- Step 1: refused in the drawer; nothing is created.
- Step 2: the create works. **Show connection** is refused with a toast of Lake's message; the delete
  is refused in the dialog (`admin` is needed for both). Delete `qa-<MMDD>-w` as `owner`.

#### SHIP-013 · No workspace yet · P2
As: newcomer

1. Open `/services`.

Expect:
- With no account: "No workspace yet" / "You have no workspace yet." with **Workspaces**, and no **New
  service**.
- With an account but no workspace: the drawer from **New service** says "Choose a workspace first"
  ("A service is created in a workspace. Choose or create a workspace to create one.") and has no
  **Create service**.

#### SHIP-014 · Services at phone width · P2
As: owner

1. At 390×844, open **Ship → Services**.

Expect:
- The figures stack; the services table scrolls inside its box; the catalog is one card per row.

## Models

#### SHIP-015 · The Models list · P1
As: owner · Needs: TD-MODEL

1. Open **Ship → Models**.

Expect:
- Title "Models", sub "Serving your models: which version answers outside calls, who may call it, and
  how it is doing."; header **Train → Models**.
- Figures "Published" ("of <n> models"), "Calls, 24 hours" ("predict calls of every model"), "Errors,
  24 hours" ("no calls" or "<x>% of calls"), "Mean latency" ("inside Lake, per call").
- The table is sorted by "Calls, 24 h", highest first. Each row: the name (a link), a kind chip, a
  "promoted" chip when an experiment promoted a version, "<source> · training"; "Serving" "not
  published" or "published" with the version chip and the model id's first 8 characters with a copy
  button; "Calls, 24 h", "Errors", "Mean latency".

#### SHIP-016 · Publish the latest version · P0
As: owner · Needs: TD-MODEL

1. On TD-MODEL's row (Serving "not published"), press **Publish v2**.

Expect:
- The button's tooltip is "Publish the latest version, v2". It spins, then Serving reads "published"
  with chip `v2`.
- **Call** and **Unpublish** replace **Publish v2**. "Published" goes up by one.

#### SHIP-017 · Call a model from code · P0
As: owner · Needs: TD-MODEL published, an API token whose scopes include `predict` or `*` (**Account → API Tokens**)

1. Press **Call** on TD-MODEL's row.
2. Copy the command, set `LAKE_KEY` to the predict key, and run it in a terminal.
3. Close the drawer and press **Call a model** in the toolbar.

Expect:
- Drawer "Call a Model from Your Code", subtitle the model's name; one block labelled with the name and
  the id's first 8 characters, holding `curl -s <origin>/v1/predict` with your account, workspace,
  model id and one sample row.
- Step 2 answers the ranked rows (`ranked`).
- Step 3: subtitle "Every published model, ready to paste." with one block per published model; with
  none published, "Nothing is published" / "Publish a model to call it from outside Lake."

#### SHIP-018 · Unpublish from the list · P1
As: owner

1. Press **Unpublish** on TD-MODEL, read the dialog, press **Cancel**.
2. Press **Unpublish** and confirm **Unpublish**.
3. Press **Publish v2** again.

Expect:
- Dialog "Unpublish <name>?" with "Unpublishing v2 stops every outside call at once. You can publish it
  again later."
- Step 2: Serving reads "not published"; the curl from SHIP-017 is now refused.
- Step 3 leaves it published for later cases.

#### SHIP-019 · Search and filter models · P2
As: owner

1. Type part of a version id in **Search models…**.
2. Press "Published", then "Not published", then press the active chip again.

Expect:
- The search matches name, kind, pipeline, project or any version id.
- The chips show their counts and filter by state; "No model matches" with "Clear the search or a
  filter to see more." when nothing is left.

#### SHIP-020 · No models yet · P2
As: owner · Needs: `W2` with no model

1. Open **Ship → Models** on `W2`.

Expect:
- No figures and no toolbar; "No models yet" with "Models appear here when a run trains one. Publish
  one to call it from outside Lake." and **Go to Train → Models**.

## Model

#### SHIP-021 · A model's serving page · P1
As: owner · Needs: TD-MODEL published at v2

1. Press TD-MODEL's name on Ship → Models.

Expect:
- Address `/w/<W1>/ship/model?id=<v2 id>`; breadcrumb Ship › Models › <name>; title "<name>" with
  chip `v2` and "published".
- Header **Training** (opens Train → Model for this version) and **Delete**.
- Tabs "Inference" (current), "Access", "Destinations", "Monitoring".
- Side: "Usage This Month" with "Predictions answered"; card "Serving" titled "v2 answers outside
  calls" with **Unpublish**; card "Facts": "Model id" (copy), "Version" "v2 of 2", "Source", "Trained",
  "Features", "Training" **Train → Models**.

#### SHIP-022 · An older version's page · P2
As: owner

1. Open v1's page (`?id=<v1 id>`).

Expect:
- Banner "v2 Is the Version Serving" with "This page is v1, which is not published." and **Open v2**.
- The Serving card reads "v1 is not published" with **Publish v1**, and "v2 is newer. Compare and
  promote it in Train."
- Inference shows "This version is not published, so `predict` refuses it. Publish it to call it."
  and **Predict** is disabled.

#### SHIP-023 · Try It · P0
As: owner · Needs: TD-MODEL published

1. On Inference, read the Endpoint card, then press **Predict** with the prefilled row.
2. Replace the rows with a list of three rows and press **Predict**.
3. Open "The whole answer".

Expect:
- Endpoint "REST" `POST <origin>/v1/predict`, "MCP" "the tool `predict` at `<origin>/mcp`",
  "model_id" with copy; then the block "Call it from your code".
- Card "Try it", title "Score a Row"; field "Rows, as JSON" with help "One object or a list of them,
  keyed by the model's features; `entity` is each row's own key."
- Step 1: a table "Row | Entity | Risk" with one row numbered `1`.
- Step 2: three rows numbered 1 to 3, highest risk first, risk to three decimals.
- Step 3 shows the answer as JSON.

#### SHIP-024 · Try It validation · P1
As: owner

1. Type `nope` and press **Predict**.
2. Type `[]`, then `[1, 2]`.
3. Type a row missing one feature.

Expect:
- "The rows are not JSON.", then "Send at least one row.", then "Each row is a JSON object keyed by the
  model's features."; no request is sent for these.
- Step 3: Lake's refusal, naming the row and the feature (for example "row 1 ('acme') has no
  'tenure'").

#### SHIP-025 · A methodology model is served by its own code · P1
As: owner · Needs: TD-MODEL-METH, not published

1. Open its page and press **Publish v<n>**.
2. Watch the Serving card until the server is serving, then press **Predict**.

Expect:
- The card says "Served by its own code, the methodology <W-…>, in its sandbox." with a state chip
  that moves through "pending", "building", "queued", "staging", "starting" to "serving" without a
  reload.
- While not serving, Inference shows the server's message or "Its server is <state>." with "`predict`
  answers once it is serving.", and **Predict** is disabled.
- When serving, **Predict** shows "Row | Entity | Prediction", one prediction per row.

#### SHIP-026 · Make a predict key · P1
As: owner

1. Open the "Access" tab (`?view=access`).
2. Press **New predict key**, clear **Name**, press **Create key**; then restore the name, set
   **Expiry in seconds** `3600` and press **Create key**.

Expect:
- Card "Keys Scoped to predict" with "A call needs a Lake key of someone with read on this workspace.
  …" and the link "Account → API Tokens".
- Dialog "New Predict Key", subtitle "A key that can call predict and nothing else. Its secret is shown
  once."; **Name** prefilled `predict <model name>`; **Expiry in seconds** help "Leave empty for as long
  as this session allows, at most 30 days."
- An empty name is refused under **Name**.
- After creating: toast "Predict key created"; banner "Copy the Key Now" / "It is shown once." with the
  secret; the table lists the key with chip `predict` and its expiry. Reloading hides the secret.
- With no key: "No predict key yet" / "Make one to call this model from your code."
- Card "Allowed origins" says "A call carrying a key is answered from any origin. …"

#### SHIP-027 · Destinations · P2
As: owner

1. Open the "Destinations" tab.

Expect:
- "Where its answers go": "Each answer goes back to the caller that asked, and nowhere else. Lake keeps
  no copy of the rows sent or the scores answered."
- "Apps" with **Ship → Apps** and "No app is bound to a model. …"; "Topics and webhooks": "Lake does
  not send a model's answers to a topic or a webhook yet."

#### SHIP-028 · Monitoring · P1
As: owner · Needs: calls made in SHIP-017 and SHIP-023

1. Open the "Monitoring" tab.
2. Press each range: "1 hour", "24 hours", "7 days", "30 days".

Expect:
- Figures "Calls" ("of this version, last 24 hours"), "Errors" ("<rate> · <n> refused, <m> failed" or
  "no calls"), "Mean latency", "Workspace predict requests" ("every model, as Lake counted them").
- Charts "Calls and errors" and "Latency", or "No calls in this range."
- "Recent calls": up to 20 minutes with calls (header "Minute", or "Starting (<n> min)" on wider
  ranges), with Calls, Errors, Mean latency; or "No calls yet" / "Calls appear here within a minute of
  being made."
- Banner "Prediction Drift Is Not Measured Yet" ending "Lake counts calls by the minute; it keeps no
  row a caller sent."
- The tab stays in the address (`view=monitoring`); "Inference" removes it.

#### SHIP-029 · An unknown model · P2
As: owner

1. Open `/ship/model?id=qa-nope`, then `/ship/model`.

Expect:
- "No such model" with "This workspace has no model version `qa-nope`." (then `(none named)`) and
  **Every model**; no **Delete** button.

#### SHIP-030 · A reader on Models · P1
As: member · Needs: TD-MODEL

1. On `W1`, open **Ship → Models**, press **Unpublish** or **Publish v<n>** on a row.
2. Open the model, press **Predict** on Inference.

Expect:
- Step 1 is refused with Lake's message in the error block above the table; the row is unchanged.
- Step 2 works for a published version (a reader may call `predict`).

#### SHIP-031 · Delete a model version, wrong name then right · P1
As: owner · Needs: a model version this run may lose (train an extra version, or v1 of TD-MODEL at
the end of the run)

1. On the version's page, press **Delete**.
2. Type `qa-wrong`, then the model's name, and press **Delete model version**.

Expect:
- Dialog "Delete <name> v<n>" with "Deleting this version removes the trained model from the
  workspace. It cannot be undone; its other versions stay." and the lines "1 trained model", "It is
  published: its endpoint stops answering." (only if it is), "Kept: the run that trained it."
- The text to type is the model's name (not "<name> v<n>"); the button stays disabled until it
  matches.
- Toast "Deleted <name> v<n>"; Ship → Models opens; the other version remains.

## Search Indexes

#### SHIP-032 · No search indexes yet · P2
As: owner · Needs: `W2` with no index

1. Open **Ship → Search Indexes** on `W2`.

Expect:
- Title "Search Indexes", sub "Search indexes over your data, and whether each is published for
  queries."
- "No search indexes yet" with "Make one over a dataset, then search it here and from your own code."
  and **New index**.

#### SHIP-033 · New Index with no datasets · P2
As: owner · Needs: a workspace with no dataset

1. Press **New index**.

Expect:
- Drawer "New Index", subtitle "A full-text (BM25F) index over the dataset."; "Loading your datasets…"
  briefly, then "No datasets yet" / "To make an index, first add a dataset on Data." with **Go to
  Data**. There is no **Create index** button.

#### SHIP-034 · Create an index · P0
As: owner · Needs: TD-DATASET

1. On `W1`, press **New index**.
2. Choose **Dataset** `qa_<MMDD>_accounts`; type `plan, account_id` in **Fields to search, comma
   separated**.
3. Press **Create index**.

Expect:
- The Dataset list puts run outputs under "Results". The fields box shows "all columns" and the help
  "Leave it empty to search all columns."
- The drawer closes and the new index's page opens: "Index on qa_<MMDD>_accounts", "not published".

#### SHIP-035 · The Search Indexes list · P1
As: owner

1. Open **Ship → Search Indexes**.
2. Type `accounts` in **Search indexes…**, then `zzz`.

Expect:
- Figures "Indexes", "Datasets covered", "Fields indexed" ("across every index").
- Newest first. Each row: the table name (a link), the id's first 8 characters and the backend; a Kind
  chip; up to four field chips and "+<n>"; "Created"; **Search**.
- A kind filter with "Every kind" shows only when there are two kinds.
- `zzz` shows "No index matches".
- Under the table: "Open an index to search it, publish it, and call it from your own code."

## Search Index

#### SHIP-036 · Try a search · P0
As: owner · Needs: the index from SHIP-034

1. On the index page, type a plan name in **Search for**, keep **In** "all fields" and **Hits** `10`,
   press **Search**.
2. Choose **In** `plan` and search again.

Expect:
- Before searching: "Ask the index something" / "Results appear here, ranked, with each matched row's
  fields and its score."
- Step 1: the address gains `q`, `field` and `top_k`; the card shows "<n> hits in <ms> ms"; each hit is
  numbered from 1 and shows the row's `plan` and `account_id` (the index's order) and a score bar.
- Step 2 limits matches to `plan`.
- Reloading the address shows the same hits.

#### SHIP-037 · Search edge cases · P2
As: owner

1. Search for `qzxqzx`.
2. Open the address with `top_k=0`, then `top_k=500`, then `top_k=abc`.
3. Clear **Search for** and press **Search**.

Expect:
- Step 1: "Nothing matched" / "Try other words, or search all fields."
- Step 2: at most 1, then at most 100, then 1 hit is asked for (the field shows the clamped value).
- Step 3: the browser stops the empty search.

#### SHIP-038 · Publish an index and make a search key · P0
As: owner

1. Read the "Publishing" and "Search Key" cards, then press **Publish**.
2. Press **Create a search key** and copy it.
3. In "Call It from Your Code", copy the curl, set `LAKE_SEARCH_KEY`, and run it.

Expect:
- Before publishing: "Only published indexes answer outside calls. Searching here works either way.";
  the key card and the code card both say "Publish the index first: only published indexes answer
  outside calls."
- After **Publish**: the title chip and the card read "published", "Answering outside calls with a
  search key."
- Step 2: the key shows once under "Your search key", with "The key keeps working for other published
  indexes until you revoke it on Tokens. …"
- Step 3: tabs "curl", "JavaScript", "Python"; the curl answers `hits` with `id`, `score`, `fields`.
  The text says "From an AI agent, the same search is the MCP tool `search_index` at `<origin>/mcp`."

#### SHIP-039 · Unpublish an index · P1
As: owner

1. Press **Unpublish** and confirm.
2. Run the curl from SHIP-038 again; then search on the page.

Expect:
- Dialog "Unpublish this index?" with "Unpublishing the index stops every outside call at once.
  Searching here keeps working, and you can publish it again later."
- The curl is refused as not published; the page's search still works.

#### SHIP-040 · Facts and an unknown index · P2
As: owner

1. Read the "Facts" card.
2. Open `/ship/index?id=qa-nope`, then `/ship/index`.

Expect:
- "Id" (12 characters and a copy button "Copy the index id"), "Table", "Kind", "Backend", "Fields"
  chips, "Created". Header **All indexes** returns to the list.
- Step 2: "Index not found" / "This index could not be read from the workspace."; no **Delete**.

#### SHIP-041 · Delete a search index, wrong text then right · P1
As: owner · Needs: the index from SHIP-034

1. Press **Delete**.
2. Type `qa-wrong`, then `qa_<MMDD>_accounts`, and press **Delete index**.

Expect:
- Dialog "Delete the Index on qa_<MMDD>_accounts" with "Deleting the index removes it from the
  workspace. It cannot be undone." and "1 search index", "It is published: its endpoint stops
  answering." (only if it is), "Kept: the dataset it indexes."
- The button stays disabled until the table name matches.
- Toast "Search index deleted"; the list opens; the dataset still exists in Data → Datasets.

#### SHIP-042 · A reader on Search Indexes · P2
As: member

1. On `W1`, press **New index** and create one; then on an index press **Publish**.
2. Search on the index page.

Expect:
- Step 1 is refused with Lake's message in the drawer and in the Publishing card.
- Step 2 works.

## Packages

#### SHIP-043 · The Packages page · P1
As: owner

1. Open **Ship → Packages**.

Expect:
- Title "Packages", sub "Container images, npm packages and Python distributions published to this
  Lake. Private images are visible only to their workspace."
- Figures "Container images", "npm packages", "Python distributions", "Image builds".
- Tabs "Container Images" (current), "npm", "Python", "Builds", each with a count. Choosing one puts
  `?tab=` in the address (none for the first).
- When the Lake has no registry storage: "No Registry Storage" / "This Lake has no registry storage
  configured." and nothing else.

#### SHIP-044 · A published package and its install command · P1
As: owner · Needs: an image pushed to the account (a repository's **Build image**, see
[05-data-repositories.md](05-data-repositories.md), or `docker push`); an npm or Python package
published by the operator, if any

Publishing has no UI: images arrive by a build or `docker push`, and npm and Python packages need the
operator's administrator credential.

1. On "Container Images", find the image; on "npm" and "Python", find any package.
2. Copy an **Install** command and run it.

Expect:
- Each package is a card with its name, its version count, a chip per version, and an "Install" block:
  `docker pull <host>/<name>:<latest>`, `npm install <name> --registry <origin>` or `pip install <name>
  --index-url <origin>/simple/`.
- Names starting `cache/` are not listed.
- The `docker pull` works after `docker login <host> -u lake` with a `registry_pull` key; the npm and
  pip commands install.
- An empty tab shows "Nothing Published" / "None published."

#### SHIP-045 · Search packages · P2
As: owner

1. On "Container Images", type part of a name in the search box, then `zzz`.

Expect:
- The placeholder is "Search container images…" ("Search npm packages…", "Search python
  distributions…" on the others); "<shown> of <total>" beside it.
- `zzz` shows "No package matches" / "Clear the search to see more."

#### SHIP-046 · The Builds tab · P1
As: owner · Needs: a build in `W1` (a repository's **Build image**)

1. Open "Builds", then open "Live log" on a build.

Expect:
- Each build is a card "Image build" titled with its target, its phase chip, and "Digest <sha256:…>"
  once built; the footer link **Browse messages** opens Train → Streams on `build-events.<id>`.
- "Live log" shows a terminal "Build log" ("No log lines yet." until lines arrive) that fills while the
  build runs.
- With no build: "No Image Builds" / "No image builds in this workspace."

## Apps

#### SHIP-047 · No apps yet · P2
As: owner · Needs: `W2` with no app

1. Open **Ship → Apps** on `W2`.

Expect:
- Title "Apps", sub "Create, deploy and operate customer applications with live rollout status."
- Header **Build with the Application Agent** and **New app**.
- "No apps yet" / "Create one from a Lake repository or a container image." with **New app**.

#### SHIP-048 · Create an app from an image · P0
As: owner · Needs: TD-IMAGE

1. On `W1`, press **New app**, choose "From an image".
2. Type `nginx:1.27` in **Image** and press **Create app**.
3. Fill **Name** `qa-<MMDD>-img`, **Image** = TD-IMAGE, keep **Framework** `container`, set **Port** to
   the image's port, and press **Create app**.

Expect:
- Drawer "New App", subtitle "Run an app from a Lake repository or a container image."; tabs "From a
  repository" and "From an image".
- Step 2: the browser refuses the tag with "An image pinned by digest: name@sha256: and 64 hex digits".
- Step 3: the app's page opens; the phase moves from "Pending" to running/ready without a reload; the
  sub is the app's address. **Open app** opens it and asks for Lake sign-in first.

#### SHIP-049 · Create an app from a repository · P1
As: owner · Needs: TD-REPO with an app at its root

1. **New app**, keep "From a repository". Fill **Name** `qa-<MMDD>-app`, choose `TD-REPO`, keep
   **Revision** `main`, **Path** `.`, **Framework** `auto`, **Port** `3000`.
2. Press **Create app**.

Expect:
- The repository list holds the account's repositories.
- The app's page opens with Mode `runtime` and the revision `main`.

#### SHIP-050 · App name rules · P2
As: owner

1. **New app**, try **Name** `Bad_Name`, then `admin`, then `qa-<MMDD>-img` (taken).

Expect:
- `Bad_Name` is stopped by the browser (pattern `[a-z][a-z0-9-]{0,30}`).
- `admin` is refused by Lake: "admin is reserved" in the drawer.
- The taken name is refused with `conflict`.

#### SHIP-051 · The Apps list · P1
As: owner · Needs: the apps above

1. Open **Ship → Apps**.
2. Search `img`, then pick a phase chip.

Expect:
- Figures "Apps", "Ready" ("of <n>"), "Needs attention" ("failed or degraded"), "With Workspace Data"
  ("bound to the workspace"). A Degraded (crash-looping) app counts in "Needs attention" (lake#1208).
  Selecting it shows only Failed and Degraded apps, with "Showing the apps that need attention. Show
  all apps".
- One card per app: label "From a repository" or "From an image", the name, a phase chip, the address
  (or "Not assigned"), "Revision" (revision or image), "Bindings" ("None" or "Workspace data, <n>
  service(s)"), footer "Updated <time>" and **Open**.
- A search or chip with no match: "No app matches these filters" / "Clear a filter to see more."

#### SHIP-052 · Build an app with the Application Agent · P1
As: owner · Needs: `agent-application` deployed, TD-DATASET

1. Press **Build with the Application Agent**.
2. In **Description** type `qa-<MMDD> a page listing the accounts most at risk from
   qa_<MMDD>_accounts`, press **Start the Application Agent**.
3. Wait.

Expect:
- Drawer "Build an App With the Application Agent", subtitle "Describe the app. The agent proposes
  files and app settings for you to confirm."
- The address gains `?task=`; card "Application Agent Is Working" with "It keeps working if you leave;
  this page checks again every 2 seconds." and **View this turn in Streams**.
- Then either a question with **Reply** (SHIP-053) or a card "Proposed App: <name>" with the reply, rows
  "Mode", "Framework", "Access", "Repository", "Path", and every file in full.

#### SHIP-053 · Reply to the agent · P2
As: owner · Needs: a task that asks a question

1. Type an answer in **Reply** and press **Reply to the Application Agent**.

Expect:
- A new `?task=` and the working card again, in the same conversation.

#### SHIP-054 · Confirm a proposal, and a public one · P1
As: owner · Needs: a proposal from SHIP-052 (ask for a public app once to get "Public — no sign-in")

1. On a private proposal, press **Confirm and deploy**.
2. On a public proposal, press **Confirm and deploy** without ticking the box; then tick "I understand
  this app will be public" and press it again.

Expect:
- Step 1: a second turn starts; its result card "Application Agent Result" shows **Open the app**; the
  app appears in the list.
- Step 2: "Access" reads "Public — no sign-in" with the warning "This app will be public: anyone with
  its address can open it without signing in to Lake." Without the tick the browser stops the form;
  with it the turn starts.
- A proposal that changed since it was shown is refused with "The Application Agent proposal changed
  before confirmation."

#### SHIP-055 · The agent is not running · P2
As: owner · Needs: a Lake without `agent-application`

1. Press **Build with the Application Agent**.

Expect:
- **Description** and **Start the Application Agent** are disabled; the notice "The Application Agent
  is not running on this Lake."

#### SHIP-056 · Another kind of task in the address · P2
As: owner · Needs: any task id that is not an Application Agent app task

1. Open `/ship/apps?task=<that id>`.

Expect:
- "That is not an Application Agent app task." above the cards.

## App

#### SHIP-057 · An app's page · P1
As: owner · Needs: `qa-<MMDD>-img`

1. Open the app from its card.

Expect:
- Address `/w/<W1>/ship/app?customer_id=…&app_id=<id>`; eyebrow "Ship · Apps"; title the name with a
  phase chip; breadcrumb Ship › Apps › <name>.
- Header **Open app** and **Event stream** (opens Train → Streams on `app-events.<id>`).
- Side cards "Rollout" (Phase, Desired, Generation `<g> / observed <o>`, URL, Mode, Framework, Port,
  Access "Lake sign-in", Updated), "Replicas", "Deploy and Lifecycle", usage this month (CPU, RAM, GPU,
  Instance time), "Delete App".
- Tabs "Overview", "Builds", "Logs", "Events", "Settings", "Services", with counts. Each tab is kept in
  `?tab=`.

#### SHIP-058 · Scale, restart, stop and start · P1
As: owner

1. Set **Replicas** `2` and press **Scale**.
2. Press **Restart**, then **Stop**, then **Start**.

Expect:
- Toasts "Scaled", "Restart requested", "Stop requested", "Start requested".
- "Generation" goes up with each change and "observed" catches up; the phase follows live in the
  title and the "Latest events" terminal.
- After **Start** the replica count is back to 2.

#### SHIP-059 · Deploy, history and roll back · P1
As: owner · Needs: a second digest of the image

1. In **Deploy image**, put the second digest and press **Deploy**.
2. On "Overview", find the first deploy in "Deploy History" and press **Rollback**, then **Roll back**.

Expect:
- A tag in **Deploy image** is refused by the browser (the digest rule).
- Toast "Deploy requested"; a "deploy" row appears with its generation, actor and target.
- Dialog "Roll Back the App?" with "The app is deployed again at `<image>`."; toast "Rollback
  requested"; a "rollback" row appears (warning tone).
- With no history: "No deployments recorded yet."

#### SHIP-060 · Builds on a runtime app · P1
As: owner · Needs: `qa-<MMDD>-app`

1. On `qa-<MMDD>-app`, open "Builds" and press **Build**.
2. Open "Logs" on the build; wait until it finishes; press **Deploy build**.
3. Press **Rebuild**, then **Cancel build** on the new one.

Expect:
- "Image builds of this app's repository."; with none, "No builds yet" / "Build the app to produce its
  first image."
- Toast "Build queued"; a card with the build id, phase chip and revision; its "Build log" fills live;
  **Browse build stream** opens Train → Streams.
- **Deploy build** shows only once the build has a digest-pinned image; it toasts "Deploy requested".
- **Cancel build** toasts "Build cancellation requested" and the phase turns cancelled.
- On a Lake without builds: "Builds unavailable" / "Builds are not available on this Lake yet." with a
  disabled **Build**.

#### SHIP-061 · Logs and events · P1
As: owner

1. Open "Logs" and press **Refresh**.
2. Open "Events", note the last sequence number, reload the page, and open "Events" again.

Expect:
- Terminal "Logs · last 200 lines": lines `<time> <pod>/<container> <line>`; "No log lines yet." when
  empty; "Logs are not available on this Lake yet." on a Lake without logs.
- Terminal "Live events" with **Browse app event stream**; "Waiting for app events…" when there are
  none. After the reload new events continue after the stored sequence, with no duplicates.

#### SHIP-062 · Environment variables and a secret · P0
As: owner · Needs: TD-SECRET (mount id, secret `crm`, field `token`)

1. Open "Settings". Fill **Key** `QA_TITLE`, **Value** `Weekly risk`, press **Set**.
2. Fill **Key** `CRM_TOKEN`, **Secret mount ID** = TD-SECRET's mount id, **Secret name** `crm`,
   **Field** `token`, press **Set**.
3. Press **Unset** on `QA_TITLE`.

Expect:
- Toast "Set QA_TITLE"; the form clears; the table shows `QA_TITLE` with `Weekly risk`.
- Toast "Set CRM_TOKEN"; the row reads `crm → token` with a "secret" chip. The token's value is
  nowhere on the page or in the Network panel.
- Toast "Unset QA_TITLE"; the row goes. With none: "No environment variables set."
- The help reads "For a secret, name a field of a secret in a secret mount of this workspace instead of
  a value. …"

#### SHIP-063 · Environment refusals · P2
As: owner

1. **Key** `QA_PW`, **Value** `postgres://user:hunter2@db:5432/x`, press **Set**.
2. **Key** `QA_BAD`, a real mount id, **Secret name** `crm`, **Field** `nope`, press **Set**.

Expect:
- Step 1: Lake refuses with `secret_required` at the top of the page.
- Step 2: Lake refuses because the secret has no such field. Neither row is added.

#### SHIP-064 · Authentication · P1
As: owner

1. In "Authentication", choose **Access** "Public" without ticking "Confirm public access", press
   **Change authentication**.
2. Tick it and press again.
3. Open the app's address in a private window, then set it back to "Lake Sign-In".

Expect:
- Step 1: Lake refuses (`invalid_input` at `/confirm_public`).
- Step 2: toast "Authentication changed"; "Access" in Rollout reads "Public".
- Step 3: the private window opens the app without signing in; after switching back it asks for Lake
  sign-in.

#### SHIP-065 · Bindings and storage · P1
As: owner · Needs: TD-SERVICE, TD-STORAGE (a volume not mounted elsewhere)

1. Open "Services". Press **Attach workspace data**, then **Detach workspace data**.
2. Choose TD-SERVICE in **Service** and press **Attach service**; then **Detach** on it.
3. Press **Choose storage**, choose the volume at a path, press **Save storage**.
4. In "Create and Attach", fill **Name** `qa-<MMDD>-appdb`, **Kind** `postgres`, **Size** `5`, press
   **Create and attach**.

Expect:
- Step 1: toasts "Workspace data attached" (chip "Attached") and "Workspace data detached".
- Step 2: toasts "Service attached" (the row shows its name and "postgres · <id>") and "Service
  detached" ("No services attached.").
- Step 3: "Saving rolls the app's pods with the storage you choose."; toast "Storage saved"; the card
  lists the volume as "Volume at <path>".
- Step 4: toast "Service created and attached"; the service appears on Ship → Services. Delete it
  there afterwards.

#### SHIP-066 · Delete an app, wrong name then right · P1
As: owner · Needs: `qa-<MMDD>-img`

1. In "Delete App", type `qa-wrong` under **App name** and press **Delete app**.
2. Type `qa-<MMDD>-img` and press **Delete app**.

Expect:
- The card says "Type `qa-<MMDD>-img` to delete".
- Step 1: Lake refuses; the error shows at the top of the page; the app stays.
- Step 2: toast "Deletion of qa-<MMDD>-img requested"; "Desired" reads deleted; the app leaves the
  list once the operator removes it.

#### SHIP-067 · A reader on an app · P1
As: member · Needs: an app in `W1`

1. Open the app. Press **Scale**, **Restart**, **Set** a variable, and **Attach workspace data**.

Expect:
- Every tab and the logs are visible.
- Each write is refused with Lake's message at the top of the page; nothing changes after a reload.

#### SHIP-068 · App without an id · P2
As: owner

1. Open `/ship/app`.

Expect:
- The address is replaced by Ship → Apps.

#### SHIP-069 · Apps and an app at phone width · P2
As: owner

1. At 390×844, open **Ship → Apps**, press **New app**, then open an app and its "Settings".

Expect:
- Cards are one column; the drawer fills the screen.
- On the app page the tabs scroll or wrap without a page scroll; the side cards are below the tabs;
  the environment form stacks one field per line.

## Deployments

#### SHIP-070 · Deployments not set up · P1
As: owner · Needs: a Lake with `deployments: false`

1. Open **Ship → Deployments**.

Expect:
- "Deployments Are Not Set Up on This Lake" with "An operator turns them on by connecting this Lake to
  the cluster operator's platform source. Until then, Lake has no environments to show."
- The Network panel shows `server_info` and no `list_repositories` or `list_environments` request.

#### SHIP-071 · Every environment · P0
As: owner · Needs: `deployments: true`, TD-DEPLOY

1. Open **Ship → Deployments**.

Expect:
- Address `/deployments`; title "Deployments", sub "Every environment across the repositories you can
  open: what is running, which change it runs, and where it is served."
- Figures "Environments", "Repositories" ("with an environment"), "Healthy", "Failing".
- The table: "Environment" (its name, a link, and the repository under it, also a link), "Phase",
  "Change" (first 12 and last 7 characters), "URL", "Updated".
- "Reading more repositories…" shows while other repositories are still being read.

#### SHIP-072 · Search, filter and nothing deployed · P2
As: owner, then newcomer

1. As `owner`, search `staging`, then `zzz`; pick a phase chip.
2. As `newcomer` (no repository with an environment), open the page.

Expect:
- `zzz` shows "No Environment Matches" / "Clear a filter to see more."
- Step 2: "Nothing Deployed Yet" with "Deploy a repository with `liquid deploy`, and its environments
  appear here."

## Environments

#### SHIP-073 · A repository's environments · P1
As: owner · Needs: TD-DEPLOY

1. Press the repository link on a Deployments row (or open `TD-REPO` and its **Deployments** tab).

Expect:
- Address `/environments?repository_id=<id>`; eyebrow "Data", title "Environments", sub "Deployments
  of `<repo>`, mirrored from the operator control plane."; breadcrumb Repositories › <repo> ›
  Environments.
- The repository tabs with **Deployments** current.
- Figures "Environments", "Healthy", "Failing"; search and phase chips; rows with Environment (and URL
  or "No URL yet"), Phase, Change, Image (`sha256:` and 12 characters), Domains, Updated.
- A repository with none: "No Environments" / "No environments have been deployed for this repository.
  Deploy it with `liquid deploy`."
- `/environments` without an id goes to Repositories.

#### SHIP-074 · Stale platform data · P2
As: owner · Needs: the operator's control plane unreachable (ask the operator; never cause it by hand)

1. Open Deployments, then an environment.

Expect:
- "Platform data is stale. The operator control plane has been unreachable since <time>. Writes are
  disabled."
- A change on the environment page answers `store_unavailable`.

## Environment

#### SHIP-075 · An environment's page · P1
As: owner · Needs: TD-DEPLOY

1. Open the environment from Deployments.

Expect:
- Address `/environment?repository_id=<id>&name=<env>`; eyebrow "Data · Environment"; title the
  environment with its phase chip; sub its URL (or "URL pending"). Header **View logs**.
- Side "Current Deployment" (Phase, Change, Image, Namespace, URL or "Pending") and "Container Logs"
  ("Follow this environment's logs live." with **View logs**).
- "Environment Variables" (or "No variables set."), "Domains" (or "No custom domains."), "Deployment
  History" (Change with message, Phase, Image, Updated; or "No deployments recorded.").

#### SHIP-076 · Set and unset an environment variable · P1
As: owner

1. Fill **Key** `QA_LOG_LEVEL`, **Value** `info`, press **Set variable**.
2. Press **Unset** on it.
3. Fill **Key** `QA_PW`, **Value** `postgres://u:hunter2@db/x`, press **Set variable**.

Expect:
- Toast "Set QA_LOG_LEVEL"; the form clears; the row appears with its time.
- Toast "Unset QA_LOG_LEVEL" (no dialog).
- Step 3: refused ("use a sealed secret") at the top of the page.
- Both fields are required by the browser.

#### SHIP-077 · Add and delete a domain · P1
As: owner

1. Fill **Host** `qa-<MMDD>.example.com`, keep **TLS** "ACME", press **Create domain**.
2. Try **Host** `not a host`.
3. Press **Delete** on the new domain, then **Delete domain**.

Expect:
- Toast "Domain qa-<MMDD>.example.com requested"; the row shows Host, TLS `acme`, a status chip, and
  the operator's message.
- Step 2: refused at `/host`.
- Dialog "Delete the Domain?" with "`qa-<MMDD>.example.com` stops serving this environment."; toast
  "Deletion of qa-<MMDD>.example.com requested".

#### SHIP-078 · Environment permissions · P2
As: member · Needs: `read` (not `write` or `deploy`) on the environment

1. Set a variable; create a domain.

Expect:
- Both are refused with Lake's message at the top of the page.

## Logs

#### SHIP-079 · Follow an environment's logs · P1
As: owner · Needs: TD-DEPLOY

1. Press **View logs**.
2. Wait for new lines; press **Refresh**.
3. Press **Environment**.

Expect:
- Address `/logs?repository_id=<id>&environment=<env>`; eyebrow "Data · Environment logs"; title the
  environment; sub "The environment's container logs, followed every 3 seconds."
- Terminal "<env> · logs" with a live dot; lines `<time> <pod>/<container> <line>`; new lines append
  every 3 seconds; empty: "No log lines yet. New lines appear here as they are written."
- **Refresh** reopens the page with `since=<cursor>`.
- **Environment** returns to the environment's page.

## Export

#### SHIP-080 · Request a repository export · P1
As: owner · Needs: TD-REPO

No page links here: open it by its address.

1. Open `/exports?repository_id=<TD-REPO id>`.
2. Press **Request and download export**.
3. Tick **Include Git projection** and press it again.

Expect:
- Eyebrow "Data · Portability", title "Repository Export", sub "Download a signed archive for
  `qa-<MMDD>-repo`."
- Side card "Request an Export" with "The archive is signed and downloads as soon as it is ready. Use
  the button above to request it."
- Step 2 downloads an archive (a GET to `/exports/download`, not a `/v1` call).
- "Prior Exports" lists both, newest first: Created, Git "omitted" then "requested", Manifest
  `lake_export_manifest/1`. With none: "No Exports Yet" / "No exports requested."
- `/exports` without an id goes to Repositories. Delete the downloads in clean-up.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/apps/AppPage.tsx:187`, `:431` | Access shows "Public" only for `auth: "none"`; an app made with `auth: "public"` reads "Lake sign-in", and the Access select matches no option | SHIP-054 (a public agent app) |
| 2 | `frontend/src/pages/ship/ServicesPage.tsx:527` | **Delete <name>** is never disabled; a wrong name is sent and refused by Lake. Other typed deletes disable until the text matches | SHIP-010 |
| 3 | `frontend/src/pages/apps/AppPage.tsx:268` | **Delete app** is never disabled for a wrong name; same as above | SHIP-066 |
| 4 | `frontend/src/pages/apps/AppPage.tsx:329`, `:362` | Any `list_builds` or `app_logs` error reads "…not available on this Lake yet.", hiding permission errors and timeouts | SHIP-060, SHIP-061 (block the request) |
| 5 | `frontend/src/pages/apps/AppPage.tsx:772` | **Cancel build** shows on finished builds too | SHIP-060 |
| 6 | `frontend/src/pages/apps/PackagesPage.tsx:178` | Every build card keeps a long poll open for as long as the tab is open, even after the build finished | SHIP-046 (Network panel) |
| 7 | `frontend/src/pages/apps/PackagesPage.tsx:45` | One search text is shared by the three registry tabs | SHIP-045 |
| 8 | `frontend/src/pages/apps/shared.tsx:208` | The stale banner says "Writes are disabled." but the environment's forms stay enabled | SHIP-074 |
| 9 | `frontend/src/pages/apps/EnvironmentsPage.tsx:87` | "Healthy" always has the ok tone, even at 0 | SHIP-073 |
| 10 | `frontend/src/pages/apps/ExportsPage.tsx` | No page links to Repository Export | SHIP-080 |
| 11 | `frontend/src/pages/ship/IndexesPage.tsx:190` | If `create_index` answers no `index_id`, the page opens `/ship/index?id=` ("Index not found") | SHIP-034 |
| 12 | `frontend/src/pages/ship/IndexPage.tsx:182` | "1 hits in <n> ms": no singular | SHIP-036 |
| 13 | `frontend/src/pages/ship/IndexPage.tsx:287` | The search key's `create_token` is sent with the workspace, the predict key's (`ModelPage.tsx:437`) without; check the two keys are alike on Account → API Tokens | SHIP-038 |
| 14 | `frontend/src/pages/ship/ModelPage.tsx:511` | Monitoring fixes its time window when the page opens; calls made after the first minute do not appear until the range or page changes | SHIP-028 (call, wait two minutes) |
| 15 | `frontend/src/pages/ship/ModelsPage.tsx:188-211` | "Calls, 24 h" adds up every version; "Errors" and "Mean latency" use only the served one | SHIP-015 |
| 16 | `frontend/src/pages/ship/IndexesPage.tsx:33` | **New index** shows and opens while the workspace gate blocks the page | SHIP-032 on a not-ready workspace |
| 17 | `docs/ui/apps.md:22`, `:118` | The doc names the figure "With Zetta" (the page says "With Workspace Data") and says a secret shows as `••••••` (the page shows `<secret> → <field>`) | SHIP-051, SHIP-062 |
| 18 | `docs/guide/ship.md:176` | The guide says each hit shows "the record's id and its score"; the page shows the row's fields | SHIP-036 |
| 19 | `docs/guide/apps.md:84` | The guide's button is "Reply to Application Agent"; the page says "Reply to the Application Agent" | SHIP-053 |
