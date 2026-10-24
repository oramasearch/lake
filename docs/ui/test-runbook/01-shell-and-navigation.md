# Shell and Navigation

The frame around every page: the rail, the top bar, the breadcrumb, the account menu, the
workspace picker, the command palette (top-bar search), the region menu, the theme switch, keyboard
shortcuts and the phone drawer. The rail items themselves are opened in [G-101](global-checks.md#g-101--every-rail-item-opens-its-page--p0);
the session-ended notice and Not Found are [G-104](global-checks.md#g-104--an-ended-session-is-caught-on-the-page--p0)
and [G-107](global-checks.md#g-107--an-unknown-address-shows-not-found--p1).

## Before you start

- Personas: `owner`, `member`, `newcomer`, `visitor`, `operator`.
- Data: `TD-ORG`, `TD-ORGWS`, `TD-W2`, `TD-9WS`, `TD-REPO`, `TD-DATASET` (for palette hits).
- The region cases need a Lake whose `server_info.region` is set; elsewhere they are `blocked`.
  The order-sheet case also needs `server_info.order_sheet_url` (an `https://` URL).
- The **Create account** case runs on `local` (Lake without Clerk, password sign-up open).
- Clear `localStorage` before SHELL-001 so the rail starts expanded and the theme follows the system.

## The rail

#### SHELL-001 · The rail opens the current group only · P1
As: owner · Needs: W1

1. Open **Data → Datasets**.
2. Press the **Design** group header in the rail.

Expect:
- Step 1: the Data group lists its items on a hairline beneath it; Design, Train, Ship, Network,
  Organization show only their header.
- Step 2: the browser is on `/w/<W1 id>/design/projects`; Design is open and Data is closed.
- Design, Train, Ship and Network links in the rail carry `/w/<W1 id>/`; **Workspaces** links to
  `/workspaces` without a prefix.

#### SHELL-002 · The brand link · P2
As: owner, then visitor · Needs: W1

1. As `owner`, press the "Lake Console" brand at the top of the rail.
2. As `visitor`, press it.

Expect:
- Step 1: the browser is on `/w/<W1 id>/design/projects` (see Suspected defects 1).
- Step 2: the browser is on `/`, the landing.

#### SHELL-003 · Collapse and expand the rail · P1
As: owner

1. At 1440 px wide, press **Collapse the navigation** at the foot of the rail.
2. Hover a group icon.
3. Reload.
4. Press **Expand the navigation**.

Expect:
- Step 1: the rail is 60 px wide and shows icons only; the group items under the current group are
  hidden; the brand shows only the mark.
- Step 2: the icon's tooltip is the group's name.
- Step 3: the rail is still collapsed (`localStorage["lake-rail"]` is `collapsed`).
- Step 4: names come back and `lake-rail` is `expanded`.
- Below 1024 px the collapse button is not shown.

#### SHELL-004 · The rail foot names who is signed in · P1
As: owner, then operator, then visitor

1. Read the person block at the foot of the rail for each persona.

Expect:
- `owner`: initials in a circle, the display name (or email), and "Account" under it.
- `operator`: initials "LO", "Lake operator", and "Operator".
- `visitor`: no button; the text "Lake 0.4.0" (or "Lake" when the version is unknown).

#### SHELL-005 · The account menu · P0
As: owner

1. Press the person block at the foot of the rail.
2. Press **Escape**.
3. Open it again and press outside it.
4. Open it again and press **Settings**.

Expect:
- Step 1: a menu with "Settings", "Sessions", "Tokens", a separator and "Sign out". On `local`
  "Passkeys" also appears between Sessions and Tokens; on a Clerk Lake it does not.
- Steps 2 and 3 close the menu.
- Step 4 opens `/settings` and the menu is closed.

#### SHELL-006 · The operator's account menu · P1
As: operator

1. Open the person block's menu.

Expect:
- The menu holds only "Sign out".

#### SHELL-007 · Sign out · P0
As: owner

1. Open the account menu and press **Sign out**.
2. Press Back in the browser.

Expect:
- Step 1: the browser leaves through `POST /logout` and lands on the server's page (a Clerk Lake
  goes through Clerk's sign-out). The rail then shows the visitor items.
- Step 2: no signed-in page is shown again; an account page sends to `/sign-in?next=…`.

## The top bar

#### SHELL-008 · Top bar by persona · P1
As: owner, then operator, then visitor

1. Open `/repositories` as each persona at 1440 px.

Expect:
- `owner`: breadcrumb, region (when the Lake has one), the workspace pill, a **Search** box with
  `⌘K`, the theme button, and **Ask Orama**.
- `operator`: the same without **Ask Orama**; the workspace pill only when the operator can see an
  account.
- `visitor`: breadcrumb, theme button and **Sign in**; no workspace pill, no Search box, no Ask
  Orama.

#### SHELL-009 · Create account in the top bar · P2
As: visitor · Needs: `local`

1. On `local`, open `/` at 1440 px, then at 390 px.
2. On a Clerk Lake, open `/` at 1440 px.

Expect:
- Step 1 at 1440 px: **Create account** (→ `/sign-up`) then **Sign in**. At 390 px only **Sign
  in** shows (see Suspected defects 2).
- Step 2: only **Sign in**.

#### SHELL-010 · The theme button · P1
As: owner

1. Press the theme button in the top bar.
2. Read its accessible name before and after.
3. Clear `localStorage["lake-theme"]`, reload, and change the system theme.

Expect:
- Step 1: the page switches theme; `<html data-theme>` and `localStorage["lake-theme"]` hold
  `dark` or `light`.
- Step 2: the name is "Use dark theme" in light theme and "Use light theme" in dark;
  `aria-pressed` is `true` in dark; the tooltip is "Theme".
- Step 3: with nothing stored, the page follows the system theme.

## Breadcrumb

#### SHELL-011 · Default and page breadcrumbs · P1
As: owner · Needs: W1

1. Open each page and read the breadcrumb.

| Page | Breadcrumb |
|---|---|
| `/w/<W1 id>/design/projects` | Design › Projects |
| `/workspaces` | Ship › Workspaces |
| `/search` | Data › Search |
| `/settings?tab=tokens` | Account › Tokens |
| `/sessions` | Account › Sessions |
| `/dashboard` | Home › Dashboard |
| `/` | Home › Ask Orama |
| `/docs` | Docs › (the first guide's title) |

Expect:
- Each row shows exactly that trail; every crumb but the last is a link and the last is bold.
- **Ship** on `/workspaces` links to `/ship/models`; **Account** links to `/settings`.

#### SHELL-012 · Long ids are shortened · P2
As: owner · Needs: TD-PROJECT (a run id)

1. Open `/w/<W1 id>/train/run?id=<a run's UUID>`.
2. Hover the last crumb.

Expect:
- The crumb is the first 8 characters of the id followed by "…".
- The tooltip is the whole id.

#### SHELL-013 · The breadcrumb on a phone · P2
As: owner

1. At 390 px, open `/w/<W1 id>/design/projects`.

Expect:
- Only the last crumb ("Projects") shows; no "›" separators.

## Keyboard shortcuts

#### SHELL-014 · Open search with ⌘K and / · P1
As: owner

1. On any page, press ⌘K (Ctrl+K on Windows and Linux). Close it with **Escape**.
2. Press `/`. Close it.
3. Put the cursor in a text field (e.g. Data → Search's box) and press `/`.
4. In the same field press ⌘K.

Expect:
- Steps 1, 2 and 4 open the search dialog with the cursor in its box.
- Step 3 types a `/` into the field and opens nothing.

#### SHELL-015 · G then a letter jumps to a group · P1
As: owner, then operator · Needs: W1

1. As `owner`, outside any text field, press G then R; then G then D; G then T; G then S; G then
   N; G then O.
2. Press G, wait two seconds, then press D.
3. As `operator`, press G then A, then G then O.

Expect:
- Step 1: R opens `/repositories`; D, T, S and N open the first page of Design, Train, Ship and
  Network under `/w/<W1 id>/`; O opens `/organizations`.
- Step 2: nothing happens.
- Step 3: G then A opens `/customers`; G then O does nothing.

#### SHELL-016 · Shortcuts are ignored while typing · P2
As: owner

1. Focus a text field and type `gd`.

Expect:
- The letters are typed; the page does not move.

## Workspace picker

#### SHELL-017 · The pill names the workspace · P0
As: owner · Needs: W1, TD-ORG

1. Open `/w/<W1 id>/design/projects` and read the pill.
2. Hover it.

Expect:
- The pill reads the workspace name, then "Your account" in muted text (the owner has an
  organization too), then a chevron.
- The tooltip is "<W1 name> — <owner's display name> (Your account)".
- Its accessible name is "Workspace: <the tooltip text>".

#### SHELL-018 · The menu groups workspaces by owner · P1
As: owner · Needs: W1, TD-W2, TD-ORGWS

1. Press the pill.

Expect:
- The first group is labelled "Your account" with the owner's display name (never the generated
  handle) and an initial tile; it lists W1 and W2.
- Then a group labelled "Organization" with `qa-<MMDD>-org` listing its workspace.
- The current workspace is bold, blue and has a check mark.
- The foot reads "New workspace" and "choose its owner".

#### SHELL-019 · Choosing a workspace keeps the page · P0
As: owner · Needs: W1, TD-W2

1. Open `/w/<W1 id>/train/runs?project=x`.
2. Open the picker and choose W2.
3. Open `/repositories`, open the picker and choose W1.

Expect:
- Step 2: the address is `/w/<W2 id>/train/runs?project=x`; the menu closes.
- Step 3: the address is `/w/<W1 id>/design/projects`.
- `localStorage["lake-workspace"]` holds the last id chosen.

#### SHELL-020 · New workspace from the picker · P1
As: owner

1. Open the picker and press **New workspace**.

Expect:
- The browser is on `/workspaces?customer_id=<current owner's id>&new=1` and the "New Workspace"
  dialog is open ([04-workspaces.md](04-workspaces.md)).

#### SHELL-021 · The picker closes · P2
As: owner

1. Open the picker, press **Escape**. Open it, press outside. Open it, press a rail item.

Expect:
- Each closes the menu.

#### SHELL-022 · The filter for long lists · P1
As: the persona of `TD-9WS`

1. Open the picker.
2. Type a word that appears in one workspace name only.
3. Press **Enter**.
4. Open the picker and type `zzzz`.

Expect:
- Step 1: a box "Filter workspaces" sits above the list, with the cursor in it. The list scrolls
  inside the menu.
- Step 2: only matching workspaces show; an owner with no match is hidden. Case and accents do not
  matter (`resume` finds `Résumé`).
- Step 3: the first match opens.
- Step 4: "No workspace matches “zzzz”."
- With 8 or fewer workspaces in all, no filter box shows.

#### SHELL-023 · A person with one owner sees no owner in the pill · P2
As: newcomer, after [WS-003](04-workspaces.md) gave them a workspace

1. Read the pill and open its menu.

Expect:
- The pill shows only the workspace name; the tooltip is the name alone.
- The menu foot reads "New workspace" without "choose its owner".

#### SHELL-024 · The picker on a phone · P2
As: owner

1. At 390 px, press the pill.

Expect:
- The pill shows the name only (no owner text).
- The menu spans the screen width under the top bar; each workspace row is at least 44 px tall.

## Command palette (top-bar search)

#### SHELL-025 · Go to pages · P0
As: owner

1. Press **Search** in the top bar.
2. Read the list with nothing typed.
3. Type `tok`.
4. Press **Enter**.

Expect:
- Step 1: a dialog with the box "Search everything, or go to a page…" focused, an `esc` button, and
  the foot "↑ ↓ move · ↵ open · ⌘ ↵ all results · esc close".
- Step 2: a group "Go to" with up to 14 pages, each with its group name on the right.
- Step 3: "Go to" lists "Tokens" (hint "Account") with "tok" marked. The last row is "See all
  results for “tok”".
- Step 4: `/tokens` opens and the dialog closes.

#### SHELL-026 · Go to a workspace · P1
As: owner · Needs: TD-W2

1. Open the palette and type the first letters of W2's name.
2. Choose the row whose hint is "Workspace".

Expect:
- The browser opens `/w/<W2 id>/` and then W2's Projects.

#### SHELL-027 · Places fill in as they answer · P0
As: owner · Needs: TD-REPO, TD-DATASET

1. Open the palette and type `qa-` (the run's prefix).
2. Watch the list as places answer.

Expect:
- With one character typed no place is searched; from two, each place appears with a spinner and
  "Searching…", then its heading with a count and up to four hits.
- "Repositories" lists `qa-<MMDD>-repo`; "Datasets" lists `qa_<MMDD>_accounts` with its database
  as the muted line, and the workspace name when the owner has several.
- A place that found nothing is left out. Places answer independently; a slow one does not hold
  the others.

#### SHELL-028 · Keyboard in the palette · P1
As: owner

1. Type `qa-`, then press ↓ three times and ↑ once.
2. Press **Enter**.
3. Open it again, type `qa-`, press ⌘+Enter (Ctrl+Enter).

Expect:
- Step 1: the selection moves row by row across groups and stops at the first and last rows; the
  selected row is highlighted and scrolled into view.
- Step 2: the selected item opens.
- Step 3: Data → Search opens at `/search?q=qa-&in=…` with every place ticked (the commas in `in` are encoded as `%2C`).

#### SHELL-029 · Nothing found · P2
As: owner

1. Type `zzqqxx`.
2. Press **Enter** without moving.

Expect:
- Step 1: once every place answered, "No results in any place." then "See all results for “zzqqxx”".
- Step 2: Data → Search opens with that query.

#### SHELL-030 · A failed place says so · P2
As: owner

1. In the Network panel, block requests matching `list_repositories`.
2. Open the palette and type `qa-`.

Expect:
- The Repositories group shows "Could not search repositories."; other places still answer.

#### SHELL-031 · The palette for a visitor and for a newcomer · P2
As: visitor, then newcomer (before WS-003)

1. As `visitor`, press `/` on `/docs`.
2. As `newcomer` with no workspace, open the palette and type `qa-`.

Expect:
- Step 1: the dialog opens (see Suspected defects 3). "Go to" lists Home, Public Repositories and
  Docs; typing searches no place.
- Step 2: Datasets, Transcripts, Projects, Solutions, Agents, Models and Drives are not searched.

#### SHELL-032 · Close the palette · P2
As: owner

1. Open it, press the `esc` button. Open it, press **Escape**. Open it, press the dimmed backdrop
   once.

Expect:
- Each closes it and focus returns to the control that opened it.

## Region menu

#### SHELL-033 · The region label · P1
As: owner · Needs: a Lake with a region

1. Read the top bar; hover the globe.
2. On a Lake without a region, read the top bar.

Expect:
- Step 1: a globe and the region's label (e.g. "Europe"); tooltip "Region: Europe".
- Step 2: no globe and no label.

#### SHELL-034 · The other regions · P1
As: owner · Needs: a Lake with more than one region

1. Press the region label.
2. Press **Escape**, open it again, and press another region's link.

Expect:
- Step 1: a menu "You are in <label>. Each region is a separate Lake with its own accounts." then
  one link per other region, "<label> <host>".
- Step 2: the browser goes to `https://<host>/`, not the current path.

#### SHELL-035 · The order sheet links · P2
As: owner · Needs: `server_info.order_sheet_url`

1. Open the region menu.

Expect:
- After the regions, "New setup…" (→ the sheet's origin `/`) and "On-prem…" (→ `/order/on-prem`).
- The menu opens even when there is no other region.

## The phone drawer

#### SHELL-036 · Open and close the drawer · P1
As: owner

1. At 390 px, press **Open the navigation**.
2. Press **Close the navigation**.
3. Open it and press the dimmed area beside it.
4. Open it and press **Escape**.
5. Open it and press a rail item.

Expect:
- Step 1: the rail slides in from the left, at most 85% of the width, over a dimmed page.
- Steps 2 to 5 each close it; step 5 also opens the page.

#### SHELL-037 · The top bar on a phone · P2
As: owner

1. At 390 px, read the top bar.

Expect:
- Beyond G-21: the Search control is an icon named "Search"; **Ask Orama** is an icon whose name is
  "Ask Orama"; the region shows the globe without its label.

## Screen error boundary

#### SHELL-038 · An error stays inside its page · P2
As: owner

1. Only when a page throws during the run (any case): read what it shows, then press a rail item.

Expect:
- The page area shows "This page hit an error" and "The rest of Lake still works: choose another
  page, or reload this one. The details are in the browser console." with **Reload**.
- The rail and the top bar keep working; the next page opens normally.
- Record the case that threw as a failure with the Console's "Lake screen error:" line.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/components/Shell.tsx:345` | The brand goes to the workspace's Projects, not `/`, which has been a signed-in person's home since lake#600 | SHELL-002 |
| 2 | `frontend/src/components/Shell.tsx:604` | **Create account** is hidden below 640 px, so a visitor on a phone is offered only **Sign in** | SHELL-009 |
| 3 | `frontend/src/components/Shell.tsx:180-193` vs `:380` | ⌘K and `/` open the palette for a visitor, who has no Search button | SHELL-031 |
| 4 | `frontend/src/components/Shell.tsx:389`, `CommandPalette.tsx:198` | The hint says `⌘K` on every system; Windows and Linux use Ctrl+K | SHELL-014 |
| 5 | `docs/ui/README.md:70` vs `Shell.tsx:53` | The doc lists G then D, T, S, O, A; the code also has R (Data) and N (Network) | SHELL-015 |
| 6 | `docs/ui/breadcrumb.md:301` vs `Shell.tsx:574` | The doc says phones show the last two crumbs; the code shows only the last | SHELL-013 |
