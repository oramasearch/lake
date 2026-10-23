# Workspaces

Ship → Workspaces (`/workspaces`): every workspace a person can reach, grouped by owner, with New
workspace, Rename and the typed-name deletion. Also the first workspace of a new account, and the
workspace gate: the states every Design, Train and Ship page shows while its workspace is not
ready. `/workspaces` is never under `/w/<id>/`.

## Before you start

- Personas: `newcomer` (no workspace yet), `owner`, `member`, `operator`.
- Data: W1, `TD-W2`, `TD-ORG`, `TD-ORGWS`.
- This file makes, and deletes again:
  - `TD-WS-NEW`: `qa-<MMDD>-ws` in `owner`'s own account (WS-008), renamed `qa-<MMDD>-ws2` (WS-013),
    deleted in WS-015.
  - `TD-WS-ORG`: `qa-<MMDD>-orgws` owned by `TD-ORG` (WS-010), deleted in WS-017.
  - `newcomer`'s first workspace (WS-003), renamed `qa-<MMDD>-newcomer` (WS-014); it stays for
    later files and is deleted at clean-up.
- A new workspace takes a minute or two to be Ready.
- WS-022 runs on a Lake with no workspace control plane (e.g. `local` started without one).
- WS-023 and WS-024 need states the UI cannot make; they are `blocked` unless the environment has
  them already. Never make them by hand on a cluster.

## A new account's first workspace

#### WS-001 · Workspaces with no workspace yet · P1
As: newcomer

1. Open **Ship → Workspaces**.

Expect:
- Eyebrow "Ship", title "Workspaces", sub "Isolated environments, each keeping its own projects,
  data and models apart from every other." and the primary **New workspace**.
- Stats "Workspaces" 0, "Ready" 0 ("0 not ready yet"), "Failed" 0, "Storage" "0 GiB".
- One group: the initial tile, "Your account", the newcomer's name, "0 workspaces", and a dashed
  button "New workspace · <name> has none yet".

#### WS-002 · A workspace page with no workspace · P1
As: newcomer

1. Open `/design/projects` (no `/w/` prefix).

Expect:
- A page "Choose a Workspace" with "Select a workspace above or create a new one." and the primary
  **Workspaces** (→ `/workspaces`).

#### WS-003 · Start the first workspace from Ask Orama · P0
As: newcomer

1. Open `/` and press **Start a workspace**.
2. Read the top bar.
3. Open **Design → Projects** at once.

Expect:
- Step 1: the button spins; then the home shows the new workspace's empty conversation.
- Step 2: the workspace pill names the new workspace (named after the account's handle).
- Step 3: the gate "Preparing Your Workspace" (section *The workspace gate*), until it is Ready;
  then the Projects page.
- On failure, a yellow banner with Lake's message sits above the button.

#### WS-004 · Preparing Your Workspace · P1
As: newcomer, during WS-003

1. Read the gate while the workspace is prepared, and keep the page open.

Expect:
- A spinner, "Preparing Your Workspace", a chip with the operator's phase (or "Not picked up yet"),
  "Lake is preparing a workspace of your own, where your projects, data and models live apart from
  every other customer's." then "Status: <phase>." (or "It has not been picked up yet.") and "This
  page checks again every 15 seconds."
- `workspace_status` is asked every 15 seconds; when it answers Ready the page shows itself without
  a reload.

## The Workspaces page

#### WS-005 · Open Workspaces · P0
As: owner · Needs: W1, TD-W2, TD-ORGWS

1. Press **Ship → Workspaces**.

Expect:
- Breadcrumb Ship › Workspaces. The address has no `/w/` prefix.
- Stats "Workspaces" (sub "across 2 owners"), "Ready" (sub "<n> not ready yet"), "Failed",
  "Storage" ("<n> GiB", sub "requested across workspaces"), counting every owner.
- A search box "Search workspaces…" and phase chips "All", "Ready <n>", ….
- The group "Your account" (the owner's name, never the handle) with W1 and W2, then the group
  "Organization" `qa-<MMDD>-org` with its workspace. Each group header reads "<n> workspaces".

#### WS-006 · A workspace row · P1
As: owner · Needs: W1

1. Read W1's row and hover its Created value.
2. Press its name.

Expect:
- Columns "Workspace", "Phase", "Storage", "Created" and the actions **Rename** and **Delete**.
- The name is bold; under it "<environment> · <namespace>" (and " · <region>" on a Lake with
  regions). Phase is a chip ("✓ Ready"). Storage reads "<n> GiB", or "<observed> / <requested> GiB"
  when they differ; a storage phase other than Ready shows as a chip. Created is relative ("3 days
  ago") with the full date on hover.
- Step 2 opens `/w/<W1 id>/design/projects`.

#### WS-007 · Search and filter · P1
As: owner

1. Type part of W2's name in the search box.
2. Clear it and press the "Ready" chip; press it again.
3. Type `zzzz`.

Expect:
- Step 1: only matching rows show; a group with no match is hidden.
- Step 2: only Ready rows show; pressing the chip again shows all.
- Step 3: "No Workspace Matches" / "Clear the search or the filter to see more."

#### WS-008 · New workspace · P0
As: owner

1. Press **New workspace**.
2. Read the dialog.
3. Set **Owner** to "Your account: <your name>", **Name** `qa-<MMDD>-ws`, keep **Environment**
   `dev`, and press **New workspace** in the dialog.

Expect:
- Step 2: a dialog "New Workspace" with "Lake prepares it for you; preparing it takes a minute or
  two." (on a Lake with regions: "Lake prepares it in <region>, where it stays; preparing it takes
  a minute or two."). **Owner** shows because the owner has two owners, with help "Your account, or
  an organization you belong to.". **Environment** has the help "Use dev, staging, prod or a
  lowercase DNS label."
- Step 3: the toast "Workspace created"; the dialog closes; the new row appears in "Your account"
  with a pending phase until it is Ready. The picker lists it too.

#### WS-009 · New workspace refuses bad input · P1
As: owner

1. Open **New workspace**. Leave **Name** empty and press **New workspace**.
2. Type a name, set **Environment** to `Prod`, press **New workspace**.
3. Set **Environment** to `dev` and **Name** to W1's name; press **New workspace**.
4. Press **Cancel**.

Expect:
- Step 1: the browser asks for **Name**; nothing is sent.
- Step 2: the browser refuses the environment (it must match `[a-z0-9]([a-z0-9-]*[a-z0-9])?`).
- Step 3: Lake refuses the taken name; its message sits under **Name** (or above the form when it
  names no field). The dialog stays open with the input.
- **Name** takes at most 128 characters, **Environment** at most 47.
- Step 4 closes the dialog with nothing made.

#### WS-010 · New workspace for an organization · P1
As: owner · Needs: TD-ORG

1. Open **New workspace**, choose **Owner** "Organization: qa-<MMDD>-org", **Name**
   `qa-<MMDD>-orgws`, and create it.

Expect:
- The toast "Workspace created"; the address gains `?customer_id=<TD-ORG id>`; the row appears in
  the organization's group.

#### WS-011 · The picker's New workspace opens the dialog · P2
As: owner

1. From the workspace picker press **New workspace**.
2. Press **Cancel**.

Expect:
- Step 1: `/workspaces?customer_id=<id>&new=1` with "New Workspace" open, its **Owner** set to the
  current workspace's owner.
- Step 2: `new` is dropped from the address; the page stays.

#### WS-012 · Sort the table · P2
As: owner

1. Press the "Workspace" header twice, then "Phase", "Storage" and "Created".

Expect:
- Each press sorts that group's table by the column, ascending then descending; the header says
  its sort (`aria-sort`).

#### WS-013 · Rename a workspace · P1
As: owner · Needs: TD-WS-NEW

1. Press **Rename** on `qa-<MMDD>-ws`.
2. Read the dialog and the **Rename** button.
3. Clear **Name**.
4. Type `qa-<MMDD>-ws2` and press **Rename**.

Expect:
- Step 2: "Rename Workspace" with "Only its name changes: its data, address and namespace stay.",
  **Name** filled with the current name and focused. **Rename** is disabled while the name is
  unchanged.
- Step 3: **Rename** stays disabled while the name is blank.
- Step 4: the toast "Workspace renamed"; the row and the picker show the new name; the id and
  namespace are the same.

#### WS-014 · Rename the newcomer's first workspace · P2
As: newcomer · Needs: WS-003

1. On Workspaces, rename the first workspace to `qa-<MMDD>-newcomer`.

Expect:
- The toast "Workspace renamed"; the pill shows the new name.

#### WS-015 · Delete a workspace: wrong name, then the right one · P0
As: owner · Needs: TD-WS-NEW (Ready)

1. Press **Delete** on `qa-<MMDD>-ws2`.
2. Read the dialog and expand "Everything by name".
3. Type `qa-<MMDD>-ws` (wrong) and press **Delete workspace and everything in it**.
4. Type `qa-<MMDD>-ws2` and press it again.

Expect:
- Step 1: the address gains `&delete=<id>`; a dialog "Delete qa-<MMDD>-ws2" with "Deleting this
  workspace destroys everything below. It cannot be undone: retained volume snapshots are the only
  copy left."
- The list: "· Everything it holds: every record, dataset, topic and store, and its own audit
  history."; "0 repositories (0 live, 0 deleted and still retained). Their retention ends now and
  their names become free."; "0 apps."; "0 unfinished builds, cancelled."; "0 image repositories,
  no longer pullable."; then "Type `qa-<MMDD>-ws2` to confirm".
- Step 2: "Everything by name" shows only when there is something to name.
- Step 3: Lake refuses; its message sits under the field and nothing is deleted (the button is
  enabled before typing, see Suspected defects 1).
- Step 4: the toast "Deleting qa-<MMDD>-ws2"; the dialog closes and the address drops `delete`.
  The row reads "Deletion requested" in place of its actions, and it is gone from the picker. Later
  it disappears from the list.

#### WS-016 · The delete dialog from its address · P2
As: owner · Needs: W2

1. Open `/workspaces?customer_id=<owner id>&delete=<W2 id>` directly.
2. Press **Escape**. Open it again and press **Cancel**.

Expect:
- Step 1: the dialog opens for W2 with its counts.
- Step 2: each closes it and the address goes back to `/workspaces?customer_id=<owner id>`; W2 is
  untouched.

#### WS-017 · Delete the organization's workspace · P2
As: owner · Needs: TD-WS-ORG

1. Delete `qa-<MMDD>-orgws` with its exact name.

Expect:
- The toast "Deleting qa-<MMDD>-orgws"; the row in the organization's group reads "Deletion
  requested".

#### WS-018 · A member's view · P1
As: member · Needs: TD-ORG, TD-ORGWS

1. Open **Ship → Workspaces**.
2. Open **Delete** on the organization's workspace, type its name and confirm.

Expect:
- Step 1: the groups the member can reach, the organization's among them.
- Step 2: Lake refuses with its message in the dialog (no write right); the workspace stays.

#### WS-019 · The operator's view · P2
As: operator

1. Open `/workspaces`.

Expect:
- With no account to hold workspaces: "No Workspace Yet" / "You have no workspace yet." and no
  **New workspace**. With accounts the operator can see: their groups.

#### WS-020 · Regions on Workspaces · P1
As: owner · Needs: a Lake with a region (and `order_sheet_url` for the last line)

1. Open **Ship → Workspaces** and read the sub-line and a row.
2. Open **New workspace**.

Expect:
- The sub-line ends "Workspaces here are in <region>."; each row's second line includes
  " · <region>".
- The dialog reads "Lake prepares it in <region>, where it stays; preparing it takes a minute or
  two."
- With an order sheet, "New setup…" and "On-prem…" sit beside **New workspace**.
- On a Lake without a region none of this shows.

## The workspace gate

The gate wraps every Design, Train and Ship page. Check it on **Design → Projects**.

#### WS-021 · Workspace Unavailable · P2
As: owner · Needs: W1

1. In the Network panel, block requests matching `workspace_status`, and open
   `/w/<W1 id>/design/projects`.

Expect:
- After the retries, "Workspace Unavailable" with "Lake could not be reached. Check your connection
  and retry."

#### WS-022 · Not Available on This Lake · P2
As: owner · Needs: a Lake with no workspace control plane

1. Open a Design page under a workspace.

Expect:
- "Not Available on This Lake" with "Design, Train and Ship are not available on this Lake yet: it
  has no workspace control plane configured."

#### WS-023 · Your Workspace Could Not Be Prepared · P2
As: a persona whose workspace the operator reported `Failed` (blocked unless the environment has
one)

1. Open a Design page in that workspace.
2. Press **Cancel workspace**.

Expect:
- A yellow banner "Your Workspace Could Not Be Prepared" with the operator's message. For a quota
  refusal: "This Lake already runs as many workspaces as it allows (workspace quota reached (<n>)).
  An operator can raise the limit; yours starts as soon as there is room, or cancel the request
  below."
- A card "Cancel This Workspace" with "Withdraw the request so you can start again later." and
  **Cancel workspace**.
- Step 2 opens `/workspaces?customer_id=<id>`.

#### WS-024 · Start Your Workspace · P2
As: a persona with a workspace that was never requested (blocked unless one exists)

1. Open a Design page in it and press **Start your workspace**.

Expect:
- Before: "Start Your Workspace" with "Design, Train and Ship run in a workspace of your own, so
  your projects, data and models are never shared with another customer. It takes a minute or two
  to prepare." and the primary **Start your workspace**.
- After: the gate shows "Preparing Your Workspace" and then the section's list page (Projects,
  Data, Agents, Runs, Models or Search Indexes).
- A refusal shows as a yellow banner above the empty state.

## Phone width

#### WS-025 · Workspaces on a phone · P2
As: owner

1. At 390 px, open **Ship → Workspaces**, then **New workspace**, then **Rename** on a row.

Expect:
- The stats wrap; each group's table scrolls inside its own box; **Rename** and **Delete** stay
  reachable.
- The dialogs fit the width with their buttons visible.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/workspaces/WorkspacesPage.tsx:524` | **Delete workspace and everything in it** is enabled before the name is typed; the shared pattern (`lib/deletionDialog.tsx:131`, `docs/ui/deletion.md` step 3) keeps it disabled until the text matches | WS-015 |
| 2 | `frontend/src/pages/workspaces/WorkspacesPage.tsx:446` | When the dry run answers without `workspace_id`, the dialog renders nothing and `?delete=` stays in the address | WS-016 |
| 3 | `frontend/src/components/WorkspaceGate.tsx:122,164` | After **Start your workspace**, navigation uses the workspace id from before the session refresh, not the id `start_workspace` answered | WS-024 |
| 4 | `docs/ui/workspace-selection.md` (How to verify), `docs/guide/workspaces.md:38` vs `frontend/src/lib/workspacePicker.ts:122-124` | The docs say a person with two workspaces and none remembered is asked to choose; the app always picks one, so "Choose a Workspace" shows only with no workspace at all | WS-002 |
| 5 | `docs/guide/workspaces.md` (Rename, Stop and Start) vs `WorkspacesPage.tsx:160` | The guide says there is no button to rename; the page has **Rename** | WS-013 |
| 6 | `docs/ui/workspace-selection.md` (The Workspaces page) | It says the figures cover every owner, then that they count the chosen customer's workspaces; the code counts every owner (`WorkspacesPage.tsx:91`) | WS-005 |
