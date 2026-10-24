# Data: Repositories

This file covers **Data → Repositories** and every page of one repository: the home (Code tab),
Files tree, File, Change Log, Change, Issues, Issue, Releases, Build Results, Build Result,
Webhooks, Branch Protection and Conflicts. It covers each viewer: the owner, a reader, a visitor on
a public repository, and the operator. A repository's **Files** tab (its Drive), sharing and share
links are in [06-data-drives-and-sharing.md](06-data-drives-and-sharing.md).

## Before you start

- Personas: `owner`, `member`, `newcomer`, `visitor`, `operator`.
- Test data: `TD-ORG`, `TD-W2`, `TD-REPO`, `TD-REPO-PUBLIC`, `TD-REPO-EMPTY`, `TD-REPO-TOMB`,
  `TD-REVIEW`, `TD-BUILDS`, `TD-WEBHOOK`, `TD-PROTECT`, `TD-CONFLICT`.
- Data this file alone needs:
  - `REPO-NEW`: the repository `qa-<MMDD>-repo-new`, made by REPO-006. It stays empty: no push,
    no issue, no webhook.
  - `REPO-MODE`: a change on `TD-REPO` that only makes one file executable
    (`chmod +x tools/run.sh`, `liquid cut "make run.sh executable"`, `liquid push`).
  - `REPO-NOORIGIN`: a Lake with neither `LAKE_EXTERNAL_ORIGIN` nor `LAKE_PUBLIC_HOST` set (the
    `local` environment usually qualifies). If none is available, REPO-015 is `blocked`.
  - A `liquid` checkout of `TD-REPO` and an API token for the *API* steps.
- The repository pages are not workspace pages: their addresses never start with `/w/`.
- Wherever a step says "the id", copy it from the page's **About** card (**Identifier**). Hover it to
  read the whole value.

## Repositories list

#### REPO-001 · The list shows the owner's repositories · P0
As: owner · Needs: TD-REPO, TD-REPO-PUBLIC, TD-REPO-EMPTY

1. Open **Data → Repositories**.

Expect:
- Eyebrow "Data", heading "Repositories", sub "Code, files and history you can access, newest first."
- The header has **New repository**.
- Four figures: **Repositories** (sub "you can read"), **Private** ("members only"), **Public**
  ("anyone can browse") and **Workspaces** ("they live in", or "<n> need a workspace").
- The table has the columns **Repository**, **Owner**, **Workspace**, **Visibility** and
  **Last change**.
- Each row's name reads `<owner>/<name>` and opens `/repository?id=<id>`. Below the name is its
  description or "No description.".
- `owner`'s personal account in the Owner column reads "<name> (you)".
- The **Workspace** column links to `/w/<id>/design/projects`.
- `TD-REPO-PUBLIC` shows a green `public` chip with a globe. The private ones show `private` with a lock.
- Under the table: "<shown> of <total> repositories".

#### REPO-002 · Search and filter the list · P1
As: owner · Needs: TD-REPO, TD-REPO-PUBLIC, TD-W2

1. Type part of `TD-REPO`'s name in **Search repositories…**.
2. Clear it. Type a word from `TD-REPO`'s description.
3. Clear it. Press a workspace chip, then press the same chip again.
4. Press the `public` visibility chip, then **Any visibility**.
5. Type `zzzz-no-match`.

Expect:
- Steps 1 and 2 show only matching rows. The count under the table follows.
- Step 3: the chips are **All workspaces** plus one chip per workspace with its count. They show
  only when the rows live in more than one workspace. Pressing the active chip again clears it.
- Step 4: only public rows, then all of them. The visibility chips sit on the right and show only
  when more than one visibility is present.
- Step 5: "No repository matches" / "Clear the search or a filter to see more."
- The search text is not in the address. A reload clears it.

#### REPO-003 · Sort the list by each column · P2
As: owner · Needs: TD-REPO, TD-REPO-PUBLIC

1. Press each column header twice: **Repository**, **Owner**, **Workspace**, **Visibility**, **Last change**.

Expect:
- Each press reverses the order. The header's `aria-sort` reads `ascending`, then `descending`.
- Hovering a **Last change** value shows "Created <date>".

#### REPO-004 · An owner link narrows the list · P1
As: owner · Needs: TD-REPO

1. In a row, press the **Owner** link.
2. Press **Show all**.

Expect:
- Step 1: the address gains `?customer_id=<id>`. A quiet banner reads "Showing the repositories of
  **<owner>**." The **Repositories** figure's sub reads "owned by <owner>". Only that owner's rows show.
- Step 2: back to `/repositories` with every row.

#### REPO-005 · The New Repository drawer and its rules · P1
As: owner

1. Press **New repository**.
2. Open the **Workspace** select.
3. Type `My_Repo` in **Repository name** and press **Create repository**.
4. Replace it with `-repo` and press **Create repository**.
5. Press **Cancel**.

Expect:
- The drawer is titled "New Repository" with the subtitle "A repository belongs to one workspace,
  and stays there."
- Fields: **Workspace** (required), **Repository name** (required, placeholder `my-first-repository`,
  help "Use lowercase letters, numbers, and inner hyphens.") and **Description**.
- Focus starts in **Repository name**.
- The **Workspace** options are grouped: "Your workspaces" first, then one group per organization.
  The current workspace is preselected.
- A quiet banner reads "New repositories are private."
- Steps 3 and 4: the browser refuses the name (pattern validation). Nothing is sent: no
  `create_repository` request appears in the Network panel.
- Step 5 closes the drawer. Nothing is created.

#### REPO-006 · Create a repository · P0
As: owner

1. Press **New repository**. Keep workspace `W1`. Type `qa-<MMDD>-repo-new` and the description
   `qa run repository`.
2. Press **Create repository**.

Expect:
- The button spins while the request runs. Then the browser is on `/repository?id=<new id>`.
- The heading reads `<owner>/qa-<MMDD>-repo-new` with a `private` chip. The sub reads "qa run repository".
- The meta line reads "Workspace <W1 name>".
- The list now has the repository at the top (newest first).

#### REPO-007 · A taken name is refused under the field · P1
As: owner · Needs: REPO-NEW

1. Press **New repository**. Type `qa-<MMDD>-repo-new` again in the same workspace's account.
2. Press **Create repository**.

Expect:
- The drawer stays open. Lake's message (`conflict`) shows under **Repository name**, in red,
  announced (`role="alert"`). Confirm the exact message with Lake.

#### REPO-008 · A person without a workspace is told to make one · P1
As: newcomer

1. Open **Data → Repositories**.
2. If **New repository** shows, press it.

Expect:
- The list is empty: "No repositories yet" / "Create your first repository to store files and
  review changes." Its **New repository** button shows only when the newcomer has an account.
- Step 2: the drawer reads "Create a workspace first" / "A repository belongs to one workspace." It
  has **Create a workspace** (opens `/workspaces`) and a footer **Close**. Confirm whether a
  newcomer with no account sees the button at all.

#### REPO-009 · A visitor sees only public repositories · P1
As: visitor · Needs: TD-REPO, TD-REPO-PUBLIC

1. Open `/repositories`.

Expect:
- Heading "Public Repositories", sub "Browse code, files, and history shared publicly on this Lake."
- No **New repository**. Three figures, with no **Workspaces** figure. The **Private** figure
  shows 0 (see defect 14).
- Only `TD-REPO-PUBLIC` is listed. `TD-REPO` is not.
- No workspace chips. The **Workspace** column is empty, and never reads "Needs a workspace".

#### REPO-010 · A visitor on a Lake with no public repository · P2
As: visitor

1. Only on a Lake where no repository is public: open `/repositories`.

Expect:
- "No public repositories yet" / "Create an account to start your own." with **Create an
  account** (opens `/sign-up`).
- Mark `blocked` when the Lake has a public repository.

## Repository home (Code)

#### REPO-011 · The home page of a repository with history · P0
As: owner · Needs: TD-REPO, TD-BUILDS

1. Open `TD-REPO` from the list.

Expect:
- Eyebrow "Data · Repository". Heading `<owner>/<name>` with the `private` chip.
- The meta line reads "Owner <link>", "Workspace <link>" and "Updated <relative>".
- The header has **Browse files**, **Build image** and **Clone** (primary).
- The tabs, in order: **Files**, **Code**, **Changes**, **Issues**, **Releases**, **Artifacts**,
  **Deployments**, **Webhooks** and **Protection**. **Code** is current.
- Four figures, each linking to its page except **References**:
  - **Files**: "<size> at main".
  - **Changes**: "latest <relative>".
  - **References**: "default main".
  - **Releases**: "latest <tag>".
- Main column:
  - **Files** ("The published tree at `main`", **Browse tree**).
  - **Recent changes** (**All changes**, at most 5 rows).
  - **README** (title "README.md").
- Side column: **Clone and push** ("Use liquid"), **About**, **References**, **Releases**
  (**All releases**), **Languages** and **Effective access** (**View access details**).

#### REPO-012 · The Files and Recent changes cards open their pages · P1
As: owner · Needs: TD-REPO

1. In **Files**, press a file, go back, then press a folder.
2. Press **All <n> paths** under the list.
3. In **Recent changes**, press a change's title.

Expect:
- At most 12 paths are listed. A file shows its size; a folder shows "directory".
- A file opens `/file?repository_id=…&ref=main&path=…`. A folder opens
  `/repository/tree?id=…&ref=main#<path>`.
- **All <n> paths** opens the tree. It shows only when there are more than 12 paths.
- A change row shows its first message line, "<author> · <relative>" and its abbreviated id. The
  title opens `/change?repository_id=…&id=…`.

#### REPO-013 · The README is rendered and raw HTML is not run · P1
As: owner · Needs: TD-REPO

1. Read the **README** card.

Expect:
- Headings, links and lists are rendered as Markdown.
- Raw HTML in the README shows as text and does not run. No script runs, and no element from it
  is inserted.

#### REPO-014 · Clone and push commands · P1
As: owner · Needs: TD-REPO

1. Read the **Clone and push** card.
2. Press **Clone** in the header.
3. Press **Copy** on each block and paste into a text editor.
4. Press **Escape**.

Expect:
- The card reads "Create an account token on this instance under **Tokens** and export it as
  `LIQUID_TOKEN` in your terminal. Use a token issued by this instance only." **Tokens** links to `/tokens`.
- The block **First checkout into a new directory** holds
  `liquid clone '<origin>/v1/repositories/<id>' './<name>'`, then `cd './<name>'` on the next line.
- The block **From that checkout, after capturing and naming your changes** holds
  `liquid push '<origin>/v1/repositories/<id>'`.
- The modal is titled "Clone This Repository" with the subtitle `<owner>/<name>`, and shows the
  same blocks.
- **Copy** turns to "Copied" for about two seconds. The pasted text matches the block exactly.
- Escape closes the modal and focus returns to **Clone**.

#### REPO-015 · Clone commands without an origin · P2
As: owner · Needs: REPO-NOORIGIN

1. On that Lake, open any repository.

Expect:
- **Clone and push** reads "An administrator must set `LAKE_EXTERNAL_ORIGIN` or
  `LAKE_PUBLIC_HOST` before clone and push commands are available." There are no copy blocks.

#### REPO-016 · An empty repository · P1
As: owner · Needs: REPO-NEW

1. Open `qa-<MMDD>-repo-new`.

Expect:
- Figures:
  - **Files** 0, "nothing published yet".
  - **Changes** 0, "none cut yet".
  - **References** 0, "none published".
  - **Releases** 0, "none published".
- **Files** card: "No files yet" / "No files are published on this ref. Push a change to publish
  its first tree."
- **Recent changes**: "No changes yet" / "No changes have been cut."
- **README**: "No README" / "Add a README.md to explain this repository."
- **References**: "No references are published." **Releases**: "No releases have been published."
- **Languages**: "No indexed file sizes yet."

#### REPO-017 · A tombstoned repository · P1
As: owner · Needs: TD-REPO-TOMB

1. Open `/repository?id=<TD-REPO-TOMB id>`.

Expect:
- Confirm with Lake whether a deleted repository still opens. If it opens:
  - A `deleted` chip sits next to the heading.
  - A red banner "Tombstoned" reads "Deleted <date and time>. Collection may reclaim unreferenced
    content after <date and time>."
  - There is no **Build image** button.
- If it does not open: the error page "We Could Not Load This Page" with the code as eyebrow and
  **Back to repositories**.

#### REPO-018 · Build an image from the repository · P1
As: owner · Needs: TD-REPO

1. Press **Build image**. Its tooltip reads "Queue an in-cluster build from this repository".

Expect:
- The button spins. Then the browser is on `/w/<the repository's workspace>/ship/packages`.
- The new build of `<name>:latest` appears on the **Builds** tab ([13-ship.md](13-ship.md)).

#### REPO-019 · A reader's Build image is refused · P1
As: member · Needs: TD-REPO

1. Open `TD-REPO`. Press **Build image**.

Expect:
- The button shows, even though `member` can only read (see defect 13).
- A red toast reads "Build image: <Lake's message>". The page stays put.

#### REPO-020 · Effective access for an owner and for a reader · P1
As: owner, then member · Needs: TD-REPO

1. As `owner`, read the **Effective access** card. Press **View access details**.
2. As `member`, open `TD-REPO` and read the same card.

Expect:
- Step 1: one row per principal (abbreviated id + role chip). The link opens
  `/repository/access?id=<id>` ([15-organization.md](15-organization.md)).
- Step 2: "Effective access is unavailable: <message>" (a reader has no `share`). Confirm the
  message. The rest of the page works.

#### REPO-021 · References and their trees · P2
As: owner · Needs: TD-REPO

1. In **References**, press `main`.

Expect:
- The first reference carries a **default** chip. Each row shows the change it points at, abbreviated.
- The link opens `/repository/tree?id=…&ref=main`.

#### REPO-022 · Languages are summed by file size · P2
As: owner · Needs: TD-REPO

1. Read **Languages**.

Expect:
- One meter per language, largest first. `TD-REPO`'s `.c` and `.cpp` files count as "C" and "C++",
  and `.md` as "Markdown".
- Under the meters: "<size> indexed across <n> files."

#### REPO-023 · An address without its id goes to the list · P2
As: owner

1. Open each in turn: `/repository`, `/repository/tree?id=x`, `/file?repository_id=x&ref=main`,
   `/changes`, `/change?repository_id=x`, `/issues`, `/issue?repository_id=x`, `/releases`,
   `/artifacts`, `/artifact?repository_id=x`, `/webhooks`, `/protection` and `/conflicts`.

Expect:
- Each one replaces the address with `/repositories`.

## Files (tree)

#### REPO-024 · The tree lists every path with its last change · P0
As: owner · Needs: TD-REPO

1. On the home page press **Browse files**.

Expect:
- The address is `/repository/tree?id=<id>&ref=main`. The heading is "Files" and **Code** is the
  current tab.
- The path bar reads `<repository> / main`. Both are links.
- The sub reads "Browse `main` by path and last change."
- The side card **This ref** lists Ref, Files, Directories, Size and Changes ("<n> in the log",
  linking to `/changes`).
- Each row has a path, "<kind> · `<mode>`" (and " · <size>" for a file), and either the newest
  change that touched it (id + "<message> · <relative>") or "No recorded change".
- The tab title reads `Lake — Code` while the heading says "Files" (see defect 17).

#### REPO-025 · Filter the tree's paths · P2
As: owner · Needs: TD-REPO

1. Type a folder name in **Filter paths…**.
2. Type `zzzz`.

Expect:
- The counter reads "<shown> of <total> paths".
- Step 2: "No path matches “zzzz”."

#### REPO-026 · A folder link jumps within the tree · P2
As: owner · Needs: TD-REPO

1. Press a folder row's path.
2. Reload the page.

Expect:
- The address gains `#<folder path>` and the folder's row scrolls into view. No new page loads.
- After the reload, the same row is scrolled into view.

## File

#### REPO-027 · A C++ file with highlighting and blame · P0
As: owner · Needs: TD-REPO

1. In the tree, press a `.cpp` file.

Expect:
- The heading is the file name. The sub reads "`<path>` at `main`".
- The path bar has the repository, `main`, each folder (each a link) and the file name (not a link).
- The header has **Raw** and **Download**.
- The facts bar reads "C++", "<n> lines", the size, "<n> changes in its blame" (or "1 change in
  its blame") and "Blob <abbreviated id>".
- Comments, strings and C++ keywords are coloured.
- Every line has a number, and the blame column shows the change id, the author and a date.

#### REPO-028 · Link to a line and to a range · P1
As: owner · Needs: TD-REPO

1. Press line number 5.
2. Shift-press line number 9.
3. Reload.
4. Press the browser's Back button.

Expect:
- Step 1: the address ends `#L5` and line 5 is highlighted.
- Step 2: the address ends `#L5-L9` and lines 5 to 9 are highlighted.
- Step 3: the range stays highlighted and is scrolled to the middle of the screen.
- Step 4 leaves the file page. Line clicks replace the history entry, they do not add one.
- Each line number's accessible name is "Line <n>".

#### REPO-029 · Raw and Download · P1
As: owner · Needs: TD-REPO

1. Press **Raw**.
2. Go back. Press **Download**. Compare the downloaded file with the original (`cmp`).

Expect:
- **Raw** opens `/file/raw?repository_id=…&ref=main&path=…` and shows the bytes as text.
- **Download** saves the file under its name, byte for byte identical.

#### REPO-030 · A plain text file and an empty file · P2
As: owner · Needs: TD-REPO

1. Open the `.txt` file.
2. Open the empty file.

Expect:
- Step 1: the language chip reads "Plain text". Nothing is coloured.
- Step 2: "0 lines" and a single row "This file is empty."

#### REPO-031 · The source view at phone width · P2
As: owner · Needs: TD-REPO

1. At 390×844, open the `.cpp` file.

Expect:
- The source table scrolls sideways inside its frame. The page itself does not (G-20 holds).
- The facts bar wraps onto several lines.

## Change Log and Change

#### REPO-032 · The change log · P0
As: owner · Needs: TD-REPO

1. Press the **Changes** tab.

Expect:
- Heading "Change Log", sub "Every change cut on this repository, newest first."
- Figures: **Changes** ("in the log"), **Authors** ("distinct identities") and **Latest** (relative;
  the sub is the date and time).
- Columns: **Change**, **Author** and **When**. **Author** and **When** sort; **Change** does not.
- Each title is the message's first line, with a leading `# ` removed, or "Untitled change". The
  id is under it.

#### REPO-033 · Search the change log · P2
As: owner · Needs: TD-REPO

1. Type a word from one change's message in **Search changes…**.
2. Type `zzzz`.

Expect:
- The counter reads "<rows> of <all>". Message, author and id are all searched.
- Step 2: "No change matches" / "Clear the search to see every change."

#### REPO-034 · An empty change log · P2
As: owner · Needs: REPO-NEW

1. Open `/changes?repository_id=<REPO-NEW id>`.

Expect:
- No figures and no search box. "No changes yet" / "No changes have been cut."

#### REPO-035 · A change and its differences · P0
As: owner · Needs: TD-REPO

1. Open the change with the multi-line Markdown message.

Expect:
- The heading is the first line without `# `. The sub has the id, "by `<author>` · <relative>",
  and a review state chip when the change has a review.
- The header has **Copy identifier** and a two-button group, **Unified** and **Side by side**.
  **Unified** is pressed.
- Figures:
  - **Files changed**: "reformat-only files hidden".
  - **Lines added**: `+<n>`, "across every hunk".
  - **Lines removed**: `−<n>`, "across every hunk"; red when above 0.
  - **Checks**.
- The **Description** card renders the Markdown body after the first line.
- **Files changed** lists each path with its status chips and "+a −r". Each path jumps to `#diff-<slug>`.
- The side cards are **Details** (Author, Created, Parents, Identifier), **Review** and **Checks and attestations**.
- The **Diff** section has one panel per file:
  - kind, old path, mode (`<old> → <new>`) and blobs.
  - Hunks headed `@@ -a,b +c,d @@`.
  - Removed lines start with `-` and added lines with `+`, each with a line-number link.

#### REPO-036 · Switch the diff layout · P1
As: owner · Needs: TD-REPO

1. Press **Side by side**, then **Unified**.

Expect:
- Side by side shows each removed line next to its added line, with no line numbers.
- The pressed button has `aria-pressed="true"`.
- The layout is not in the address. A reload shows **Unified**.

#### REPO-037 · Copy a change's id · P2
As: owner · Needs: TD-REPO

1. Press **Copy identifier** and paste into a text editor.

Expect:
- The button reads "Copied" with a check, and stays that way until the page reloads.
- The pasted value is the whole id, matching the hover text of **Identifier** in **Details**.

#### REPO-038 · A reviewed change with a comment · P1
As: owner · Needs: TD-REVIEW

1. Open the change that `TD-REVIEW` names.
2. In the comment, press "line <n>".
3. Press **Side by side** and look for the comment.

Expect:
- The sub has an `approved` chip.
- **Review** lists "approved" with a chip and "Reviewers: <ids>", or "Reviewers: None".
- The comment shows inside that file's diff panel, after the hunks: its id · "line <n>", then the
  Markdown body.
- Step 2 jumps to that added line in Unified view (the anchor `#<slug>-new-<n>`).
- Step 3: the comment is still shown. Its line link no longer has a target (see defect 4).

#### REPO-039 · A change without a review, and the checks card · P2
As: owner · Needs: TD-REPO, TD-BUILDS

1. Open a change of `TD-REPO` that has no review.

Expect:
- **Review** reads "No review has been requested."
- **Checks and attestations** lists build results. Each row has its action, a `passed` or `failed`
  chip, its id, and "<n> attestations · outputs: `<paths>` · attested: `<profiles>`".
- The list shows the repository's build results, not only this change's (see defect 3). Record
  whether a change with no results of its own still lists results.

#### REPO-040 · A change that only changes a mode · P2
As: owner · Needs: REPO-MODE

1. Open the "make run.sh executable" change.

Expect:
- The file is listed with its status chip(s). Its panel's mode reads `100644 → 100755`.
- "+0" and "−0" show. Confirm with Lake whether a mode-only change shows a `mode-changed` chip.

#### REPO-041 · Reviews unavailable while the workspace is not ready · P2
As: owner · Needs: TD-REPO

1. Only when `TD-REPO`'s workspace is starting or stopped: open any change.

Expect:
- **Review** reads "Reviews are unavailable: <reason>". The diff and every other card still show.
- Mark `blocked` when the workspace is Ready throughout the run.

## Issues

#### REPO-042 · An empty issue tracker · P2
As: owner · Needs: REPO-NEW

1. Open the **Issues** tab of `qa-<MMDD>-repo-new`.

Expect:
- Heading "Issues", sub "Work and questions tracked against this repository."
- Figures: **Open** ("need attention"), **Closed** ("resolved"), **Labels** ("in use") and
  **Unassigned** ("no one on them"). All read 0.
- The state tabs read **Open 0**, **Closed 0** and **All 0**.
- The empty state reads "No issues yet" / "Open an issue to track a bug, a task or a question.",
  with **New issue**.

#### REPO-043 · The New Issue drawer · P1
As: owner · Needs: TD-REPO

1. On `TD-REPO`'s **Issues** tab, press **New issue**.
2. Leave **Title** empty and look at **Open issue**.
3. In **Description**, type `**bold**`, press **Preview**, then **Write**.
4. Clear the description and press **Preview**.
5. In the new-label box, type `needs-triage` and press **Enter**. Type `bug` and press **Add**.
   Press the `needs-triage` chip.
6. Tick one person under **Assignees**.

Expect:
- The drawer is titled "New Issue", subtitle "Track a bug, a task or a question on this repository."
- **Title** is required, focused and has the placeholder "Import fails on an empty CSV".
- The **Description** placeholder is "What happened, what you expected, and the steps to see it."
  "Markdown is supported." shows under it.
- Step 2: **Open issue** is disabled while the title is blank.
- Step 3: the preview shows **bold** text.
- Step 4: "Nothing to preview."
- Step 5: both labels become chips. Pressing a chip turns it off. **Add** is disabled while the
  box is empty. The `bug` chip is red.
- Step 6: **Assignees** has the help "People of the repository's workspace." Each row shows a name
  and "@handle · email". The ticked row is highlighted with a check.

#### REPO-044 · Open an issue · P0
As: owner · Needs: TD-REPO

1. In the drawer, type the title `qa-<MMDD> import fails on an empty CSV`, a Markdown description,
   the label `bug`, and tick `owner` as assignee.
2. Press **Open issue**.

Expect:
- A toast reads "Issue opened". The browser is on `/issue?repository_id=<id>&id=<issue id>`.
- The heading is the title. The sub has an `open` chip, the id, "opened <relative> by <you>"
  (the viewer reads "<name> (you)" or "you") and "· 0 comments".
- **Labels** shows `bug` and **Assignees** shows the owner.

#### REPO-045 · Issue state tabs and filters stay in the address · P1
As: owner · Needs: TD-REPO, REPO-044

1. On the **Issues** tab, press **Closed <n>**, then **All <n>**, then **Open <n>**.
2. Press the `bug` chip next to "Label". Then press an assignee chip next to "Assignee".
3. Reload. Then press **Any label** and **Anyone**.
4. In the table, press the `bug` label inside a row.

Expect:
- Step 1: the address gains `state=closed`, then `state=all`. Open removes `state`.
- Step 2: the address gains `label=bug`, then `assignee=<taap id>`. The rows narrow each time.
- Step 3: the reload keeps the same tab and chips. **Any label** and **Anyone** remove them.
- Step 4 sets the label filter, as the chip did.
- With a filter that matches nothing: "No issues match these filters" / "Clear a filter to see more."
- On **Closed** with no closed issue and no filter: "No closed issues" / "Switch the tab to see the others."

#### REPO-046 · Search issues by title or id · P2
As: owner · Needs: REPO-044

1. Type part of the issue's title in **Search issues…**. Then paste the issue's id.

Expect:
- Both find the issue. The search is not in the address.

#### REPO-047 · Comment on an issue · P1
As: owner · Needs: REPO-044

1. Open the issue. Look at **Comment** with the box empty.
2. In **Add a Comment**, type `First comment with a [link](https://example.com)`. Press **Comment**.

Expect:
- Step 1: **Comment** is disabled. The empty conversation reads "No comments yet" / "Start the
  conversation below."
- Step 2: a toast reads "Comment added" and the box clears.
- The comment appears under **Conversation**, oldest first, titled "<name> commented <relative>",
  with the link rendered.
- The sub now reads "· 1 comment".

#### REPO-048 · Edit an issue's title and description · P1
As: owner · Needs: REPO-044

1. Press **Edit**. Clear **Title** and look at **Save**.
2. Type a new title and change the description. Press **Save**.
3. Press **Edit** again, change nothing, and press **Save**.

Expect:
- The card is titled "Edit Issue". **Edit** in the header is disabled while editing.
- Step 1: **Save** is disabled with a blank title.
- Step 2: a toast reads "Issue updated". The heading and body show the new text.
- Step 3: the card closes with no toast and no `update_issue` request.

#### REPO-049 · Change labels and assignees in the side cards · P1
As: owner · Needs: REPO-044

1. Press the pencil on **Labels** (named "Change labels"). Add `docs`. Press **Save**.
2. Press the pencil on **Assignees** (named "Change assignees"). Untick the owner. Press **Save**.
3. Press a pencil, then **Cancel**.

Expect:
- Step 1: a toast reads "Labels updated". The card lists `bug` and `docs`.
- Step 2: a toast reads "Assignees updated". The card reads "No one yet."
- Step 3 closes the picker with no change.

#### REPO-050 · Close with a comment, then reopen · P1
As: owner · Needs: REPO-044

1. Type `Closing: fixed` in the comment box. The state button reads "Close with comment". Press it.
2. Press **Reopen issue** in the **State** card.

Expect:
- Step 1: the comment is added, then the state changes. A toast reads "Issue closed". The chip
  reads `closed` and the box is empty.
- With the box empty, the button reads "Close issue". On a closed issue it reads "Reopen issue",
  or "Reopen with comment" when the box has text.
- Step 2: a toast reads "Issue reopened" and the chip reads `open`.
- The **Issues** tab's figures and tab counts follow.

#### REPO-051 · An unknown issue id · P2
As: owner · Needs: TD-REPO

1. Open `/issue?repository_id=<TD-REPO id>&id=00000000-0000-0000-0000-000000000000`.

Expect:
- An error page with eyebrow "not_found", heading "We Could Not Load This Page", sub "issue not
  found" and **Back to repositories**.

#### REPO-052 · A reader cannot change issues · P1
As: member · Needs: TD-REPO, REPO-044

1. Open `TD-REPO`'s **Issues** tab and hover **New issue**.
2. Open the issue from REPO-044. Hover **Edit**, the two pencils and the state button.

Expect:
- **New issue** is disabled. Its tooltip, and a muted line above the tabs, read "You can read these
  issues. Opening, commenting and changing them need write access on the repository."
- On the issue page, **Edit**, both pencils and the state button are disabled with the same
  tooltip. The comment box is replaced by a quiet banner with that sentence.
- Assignees show as abbreviated ids or names that `member` cannot look up. A reader cannot list
  the repository's people.

#### REPO-053 · A visitor reads a public repository's issues · P1
As: visitor · Needs: TD-REPO-PUBLIC (with at least one issue)

1. Open the public repository's **Issues** tab and one issue.

Expect:
- The list and the issue read normally.
- **New issue**, **Edit**, the pencils and the state button are disabled. Their reason reads "Sign
  in with write access on this repository to open, comment on or change issues."

## Releases

#### REPO-054 · Releases with attested artifacts · P1
As: owner · Needs: TD-BUILDS

1. Press the **Releases** tab.

Expect:
- Heading "Releases", sub "Published releases with their notes and attested artifacts."
- Figures: **Releases** ("latest <tag>"), **Latest** (relative; the sub is the date and time),
  **Artifacts** ("across every release") and **Attestations** ("on those artifacts").
- One card per release, newest first and highlighted:
  - The label is the release id. The title is the tag with a tag icon.
  - "change <id>" on the right.
  - "Published <date time> at <date time>" (see defect 15), then the notes as Markdown.
- The card footer is titled "Artifacts and attestations". Each row reads "<action> · <n> outputs"
  and "<id> · <n> attestations".
- The home page's **Releases** card lists the same tag.

#### REPO-055 · No releases yet · P2
As: owner · Needs: REPO-NEW

1. Open `/releases?repository_id=<REPO-NEW id>`.

Expect:
- "No releases yet" / "No releases have been published." No figures.

## Build Results

#### REPO-056 · The build results list · P1
As: owner · Needs: TD-BUILDS

1. Press the **Artifacts** tab.

Expect:
- Heading "Build Results", sub "Each build action's outputs, cache outcome and attestations."
- Figures:
  - **On this page**: "the last page" or "more on the next page".
  - **Cache hits**: "of <n>".
  - **Failed**: "non-zero exit"; red when above 0.
  - **Divergent**: "outputs disagree".
- The card **Filter build results** has **Action**, **Output path**, **Content digest**,
  **Page size** and **Filter**.
- Columns:
  - **Result**: the id as a link, with the output paths or "no outputs" under it.
  - **Action**, **Exit** (`passed` or `failed`), **Cache** (`hit`, `miss` or `divergent`) and **Observed**.
- **Back to repository** is under the table.

#### REPO-057 · Filter and page the build results · P1
As: owner · Needs: TD-BUILDS

1. Set **Page size** to `1` and press **Filter**.
2. Press **Next page**.
3. Type an existing action name in **Action** and press **Filter**.
4. Type `zzzz` in **Output path** and press **Filter**.

Expect:
- Step 1: the address carries `limit=1`. One row. **On this page** reads "more on the next page".
- Step 2: the address gains `cursor=…` and keeps the filters. The next result shows. **Next page**
  goes away on the last page.
- Step 3: only rows of that action.
- Step 4: "No build results match these filters" / "Change or clear a filter to see more."

#### REPO-058 · Out-of-range page sizes fall back to 100 · P2
As: owner · Needs: TD-BUILDS

1. Open `/artifacts?repository_id=<id>&limit=5000`, then `&limit=abc`.

Expect:
- Both list with a page size of 100. The request sends `limit=100`.

#### REPO-059 · No build results yet · P2
As: owner · Needs: REPO-NEW

1. Open `/artifacts?repository_id=<REPO-NEW id>`.

Expect:
- "No build results yet" / "Results appear here when a build runs from this repository." No figures.

#### REPO-060 · One build result · P1
As: owner · Needs: TD-BUILDS

1. In the list, press the id of the passed result.

Expect:
- The heading is the whole id, in mono. The sub has the action, `passed` and the cache chip.
- **All artifacts** opens the list.
- Side cards:
  - **Result**: Action, Exit, Cache, Divergent, Verification, Worker, Observed, Started, Finished.
  - **Timing**: meters for Queued, Sandbox, Execute, Hash and Store, in ms.
  - **Trace**: its facts, or "No retained trace contains this result."
  - **Search inside**.
- Sections:
  - **Outputs**: Path, Mode, Blob; or "No outputs.".
  - **Indexed Members**: or "This artifact has not been indexed yet.".
  - **Attestations**: Profile, Subject, Issuer, Verification; `verified`, or "unverified: <reason>".
- **Back to repository** at the bottom.

#### REPO-061 · Search inside a build result · P2
As: owner · Needs: TD-BUILDS

1. On the result page, press **Search** with **Symbol or text** empty.
2. Type `main` and press **Search**.

Expect:
- Step 1: the browser refuses (required field).
- Step 2 opens `/search?repository=<id>&query=main` ([07-data-search.md](07-data-search.md)).
- The card reads "Search an output with `in:artifact:<output>@<result>`. Members nested more than
  one container deep are recorded but not indexed."

## Webhooks

#### REPO-062 · Webhooks and their deliveries · P1
As: owner · Needs: TD-WEBHOOK

1. Press the **Webhooks** tab.

Expect:
- Heading "Webhooks", sub "Each webhook and its signed delivery history."
- Figures: **Webhooks** ("configured"), **Deliveries** ("attempts kept") and **Failed**. **Failed**
  is red, with the sub "can be retried".
- One card per webhook:
  - The label is its id, the title is its URL, and the created date and time is on the right.
  - "Events" chips.
- Each attempt shows a `delivered` or `failed` chip, the event, "attempt <n> · status <code> ·
  <error or "no error">" and its id.
- The signing secret is not shown anywhere on the page or in the `/v1/` responses.

#### REPO-063 · Retry a failed delivery · P1
As: owner · Needs: TD-WEBHOOK

1. On a failed attempt, press **Retry**.

Expect:
- The button spins. A toast reads "Delivery retried".
- A new attempt appears with the next attempt number. The old one is still listed.
- If the retry is refused, the message shows in red next to the button.

#### REPO-064 · No webhooks yet · P2
As: owner · Needs: REPO-NEW

1. Open the **Webhooks** tab of `qa-<MMDD>-repo-new`.

Expect:
- "No webhooks yet" / "No webhooks are configured." No figures.

## Branch Protection

#### REPO-065 · Rules and refused updates for the owner · P1
As: owner · Needs: TD-PROTECT

1. Press the **Protection** tab.

Expect:
- Heading "Branch Protection", sub "The rules that guard references, and the updates they refused."
- Figures: **Rules** ("protected references") and **Refused updates** ("from the workspace audit").
- Section **Rules** has a card per rule titled with its ref:
  - Review `optional`, Verification `optional`, Updates `fast-forward only`.
  - Allowed pushers "None", and the Policy object id.
- Section **Refused Updates** lists the refused push:
  - **Reference** with its reason, Outcome `refused`, **Caller**, **Rule** and **When**. **When** sorts.
- With throttling at *Slow 4G*, watch the first load (see defect 5): record whether "No protected
  updates have been refused" shows before the refusal arrives.

#### REPO-066 · A reader sees the rules but not the refusals · P1
As: member · Needs: TD-PROTECT

1. Open `TD-REPO`'s **Protection** tab.

Expect:
- The tab is there for a signed-in reader. The guide says operator only; the code is followed here
  (see defect 6).
- **Rules** lists the same rules.
- **Refused updates** reads "—", with the sub "the account's administrators can see these".
- Section **Refused Updates** reads "Refused updates are in the audit" / "The account's
  administrators can see which updates these rules refused."

#### REPO-067 · No branch protection rules · P2
As: owner · Needs: REPO-NEW

1. Open the **Protection** tab of `qa-<MMDD>-repo-new`.

Expect:
- **Rules** 0 and "No branch protection rules". **Refused Updates** reads "No protected updates have
  been refused".

## Conflicts

#### REPO-068 · The conflicts page explains each conflict · P1
As: owner · Needs: TD-CONFLICT

1. Open `TD-REPO`'s **Files** tab. Press the **⋯** menu named "Folder actions" and choose **Conflicts**.

Expect:
- The address is `/conflicts?repository_id=<id>`. No repository tab is current.
- Heading "Conflicts", sub "Concurrent values Lake preserved instead of choosing one. Compare
  them, then keep one."
- An info banner reads "1 unresolved conflict" (or "<n> unresolved conflicts") / "Each is resolved on `main`."
- Each card:
  - The label is the kind in words ("Text overlap" for `TD-CONFLICT`) and the title is the path.
  - A kind chip and **Compare preserved values** (opens `/drive/conflict?…`).
  - Body: the explanation, then "Affected region: `<path>` · byte offset <n> · length <n>".
  - **Preserved values** lists Base value, Left value and Right value.
  - Under **Choose the Value to Keep**: **Keep left value**, **Keep right value**, **Prepared
    replacement text** and **Use prepared text**.
  - **Accept reviewed proposal** shows only when the conflict has a reviewed proposal.

#### REPO-069 · Keep the left value · P1
As: owner · Needs: TD-CONFLICT

1. Press **Keep left value**.

Expect:
- Every resolution button is disabled while it runs. A toast reads "Conflict resolved".
- The conflict leaves the list. With none left: "No conflicts" / "No unresolved conflict values."
- The file at `main` now holds the left value. Check its File page.
- `TD-CONFLICT` is now used up. [06-data-drives-and-sharing.md](06-data-drives-and-sharing.md)
  needs its own conflict.

## Access by viewer

#### REPO-070 · A visitor reads a public repository · P0
As: visitor · Needs: TD-REPO-PUBLIC

1. Open the public repository from **Public Repositories**.
2. Open its tree, a file, the change log, a change, and its releases.

Expect:
- Every page loads with no sign-in prompt.
- The tabs are **Code**, **Changes**, **Issues**, **Releases** and **Artifacts** only. **Files**,
  **Deployments**, **Webhooks** and **Protection** are hidden.
- There is no **Build image** and no "Workspace" in the meta line or the **About** card.
- **Effective access** reads "No direct or inherited grants are visible." with no **View access details**.
- **Clone** and **Raw** and **Download** work.

#### REPO-071 · A visitor is never told a private repository exists · P1
As: visitor · Needs: TD-REPO

1. Open `/repository?id=<TD-REPO id>`.
2. Open `/repository?id=00000000-0000-0000-0000-000000000000`.
3. Open `/webhooks?repository_id=<TD-REPO-PUBLIC id>` and `/protection?repository_id=<TD-REPO-PUBLIC id>`.

Expect:
- Steps 1 and 2 both land on `/sign-in?next=<encoded address>`. Nothing on the way says whether
  the repository exists.
- Step 3: both are sign-in pages too, even for a public repository.

#### REPO-072 · A reader browses without write controls failing silently · P1
As: member · Needs: TD-REPO

1. Open `TD-REPO`'s home, tree, a file, a change, its releases, its build results and its webhooks.
2. On **Webhooks**, press **Retry** on a failed attempt.

Expect:
- Every page reads normally.
- Step 2: **Retry** shows to a reader (see defect 32). Lake's refusal shows in red next to the
  button, and no new attempt appears.

#### REPO-073 · The operator sees the list and a repository · P2
As: operator · Needs: TD-REPO

1. Open **Data → Repositories** and then `TD-REPO`.

Expect:
- The list loads with what the operator may read. Record whether **New repository** shows.
- The home page loads. The **Issues** tab's **New issue** is enabled for the operator.

## Phone width and keyboard

#### REPO-074 · Repository pages at phone width · P2
As: owner · Needs: TD-REPO

1. At 390×844, open the home, the tree, a change in both layouts, and the Issues tab.

Expect:
- The repository tabs stay on one row and scroll sideways.
- On the home page, the side cards (**Clone and push**, **About**, …) come after **README**.
- Figures stack one per row.
- On the tree, the "Last change" heading is hidden and each row stacks its path above its last change.
- Diff panels and the issues table scroll inside their own box.
- The **New Issue** drawer's **Open issue** stays reachable with the on-screen keyboard open.

#### REPO-075 · Repository pages by keyboard · P2
As: owner · Needs: TD-REPO, REPO-044

1. On a File page, Tab to a line number and press **Enter**.
2. On a change, Tab to **Side by side** and press **Space**.
3. On the issue page, Tab to the **Labels** pencil and press **Enter**. Then press **Escape** in
   the label box.

Expect:
- Step 1 sets `#L<n>` as a click does.
- Step 2 switches the layout. `aria-pressed` follows.
- Step 3 opens the picker. Escape does nothing there: the picker is not a dialog. Its **Cancel** closes it.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 3 | `frontend/src/pages/repositories/ChangePage.tsx:52, 88-89, 160-185` | **Checks** and **Checks and attestations** list the repository's first 100 build results, not this change's. "No checks have reported for this change." is wrong whenever the repository has any result. | REPO-039 |
| 4 | `frontend/src/pages/repositories/ChangePage.tsx:311-323` | Review comments render after the hunks, not beside their line (the guide says beside). The "line <n>" link has no target in Side by side view, or for a line that was not added. | REPO-038 |
| 5 | `frontend/src/pages/repositories/ProtectionPage.tsx:40-47` | The audit read is not awaited. While it loads, **Refused updates** shows 0 and "No protected updates have been refused". Only the latest 500 audit events are searched. | REPO-065 |
| 6 | `docs/guide/repositories.md:77`, `docs/guide/repository-settings.md:8, 46, 166` | The guide says Protection is the operator's only. The code shows it to every signed-in reader. | REPO-066 |
| 13 | `frontend/src/pages/repositories/RepositoryPage.tsx:143` | **Build image** shows to read-only viewers; the refusal is only a toast. | REPO-019 |
| 14 | `frontend/src/pages/repositories/RepositoriesPage.tsx:98` | Visitors see a **Private** figure that is always 0. | REPO-009 |
| 15 | `frontend/src/pages/repositories/ReleasesPage.tsx:86` | "Published <created> at <updated>" joins two full dates with "at". | REPO-054 |
| 16 | `frontend/src/pages/repositories/FilePage.tsx:174` | The File trail is "Repositories › <path>": no repository crumb, and the tab title is the bare path. | REPO-027 |
| 17 | `frontend/src/pages/repositories/TreePage.tsx:57` | The tree's tab title is "Lake — Code" while its heading and registry title are "Files". | REPO-024 |
| 18 | `frontend/src/pages/repositories/ArtifactPage.tsx:50-52`, `ArtifactsPage.tsx:51` | One thing has four names: tab "Artifacts", heading "Build Results", trail "Artifact", button "All artifacts". | REPO-060 |
| 20 | `frontend/src/pages/repositories/forge.tsx` `ForgeTabs`, `ConflictsPage.tsx:44` | There is no Conflicts tab. The page is reachable only from the Drive folder menu. | REPO-068 |
| 25 | `docs/ui/repositories.md:11` | The spec lists "lifecycle" on the home page. There is no rename, visibility or delete control. | REPO-011 |
| 27 | `frontend/src/pages/repositories/IssuePage.tsx:76-77` | "Close with comment" posts the comment, then changes the state. If the second call fails, the comment stays and a retry posts it again. | REPO-050 |
| 32 | `frontend/src/pages/repositories/WebhooksPage.tsx:90` | **Retry** shows to any signed-in reader; the capability needs `write`. | REPO-072 |
| 34 | `frontend/src/pages/repositories/TreePage.tsx:58-64`, `FilePage.tsx:170` | The tree and file pages wait for a diff of every change before showing anything. Long histories leave them on the skeleton. | REPO-024 |
