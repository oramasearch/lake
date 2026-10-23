# Data Search

Data → Search (`/search`): one query across the places a person ticks (repositories, code,
datasets, transcripts, projects, solutions, agents, models, drives and the docs), each answered by
the read its own page uses, side by side. The top bar's quick search is in
[01-shell-and-navigation.md](01-shell-and-navigation.md#command-palette-top-bar-search).

## Before you start

- Personas: `owner`, `member`, `newcomer` (with the workspace from WS-003), `visitor`.
- Data: W1 and `TD-W2`, `TD-REPO` (with code and `README.md`), `TD-DATASET`, `TD-PROJECT`,
  `TD-AGENT`, `TD-MODEL`, a drive with a file named `qa-<MMDD>-…` (from
  [06-data-drives-and-sharing.md](06-data-drives-and-sharing.md)), a transcript (from
  [09](09-data-connections-transcripts-mcp.md)) if one exists.
- Search words used here: `qa-` and `qa_` (the run's prefix), a word from `TD-REPO`'s
  `README.md`, and `zzqqxx` (matches nothing).
- Clear `localStorage["lake-search-places"]` before SRCH-001.

## The page

#### SRCH-001 · Open Search · P0
As: owner

1. Press **Data → Search** in the rail.

Expect:
- Breadcrumb Data › Search. Eyebrow "Data", title "Search", sub "One search across your
  repositories, code, datasets, transcripts, projects, solutions, agents, models, drives and the
  docs. Choose where to look."
- A search box "Search everything you chose below…" with the cursor in it, and **Search**.
- "Search in" with ten ticked chips: "Repositories", "Code", "Datasets", "Transcripts",
  "Projects", "Solutions", "Agents", "Models", "Drives", "Docs", and a chip "Code only".
- "Datasets, transcripts, projects, solutions, agents, models and drives are searched in all <n> of
  your workspaces." (<n> counts every workspace the owner can reach, the organization's included)
- A closed "Code options".
- The empty state "Search across Lake" / "Type what you are looking for and choose where to look.
  You can search several places at once."

#### SRCH-002 · Search everything · P0
As: owner · Needs: TD-REPO, TD-DATASET, TD-PROJECT

1. Type `qa-` and press **Search** (or Enter).

Expect:
- The address is `/search?q=qa-`.
- "Searching…" while places answer, then "<n> results for “qa-”" (or "1 result for …").
- One section per ticked place, each with its icon, name and count. "Repositories" lists
  `qa-<MMDD>-repo`; other places list their hits with the words marked.
- Workspace hits name their workspace on the right, because the owner has several.
- Each chip in "Search in" shows its count.

#### SRCH-003 · Hits open their pages · P1
As: owner · Needs: SRCH-002

1. Press a hit in each section in turn, going back each time.

Expect:
- A repository opens `/repository?id=…`; a dataset `/w/<id>/train/dataset?name=…`; a project
  `/w/<id>/design/project?name=…`; an agent `/w/<id>/design/agent?id=…`; a model
  `/w/<id>/train/model?id=…`; a transcript `/w/<id>/transcript?id=…`; a doc
  `/docs/<slug>#<heading>`; a drive hit its folder or file.
- Each opens in the hit's own workspace.

#### SRCH-004 · Code search · P1
As: owner · Needs: TD-REPO

1. Untick every place but Code (or press **Code only**).
2. Search for a word from `TD-REPO`'s `README.md`.

Expect:
- Step 1: only "Code" is ticked; the chip now reads "Everything"; the address holds `in=code`; the
  workspace sentence is gone.
- Step 2: the Code section lists `README.md` with its repository and line, the matching line as
  code below it.

#### SRCH-005 · Code options · P1
As: owner · Needs: TD-REPO

1. Open "Code options".
2. Fill **Repository** with `qa-<MMDD>-repo`, **Branch or tag** with `main`, tick **Scan complete
   history**, and press **Apply**.
3. Reload.

Expect:
- Step 1: fields "Repository", "Branch or tag", "Exact change", a checkbox "Scan complete history",
  **Apply**, and the help "Ranked text, quoted substring or /regex/. A tag uses its snapshot; a
  change or complete history scans. Add `in:artifact:<name>@<version>` to search a published
  artifact."
- Step 2: the address holds `repository=…&ref=main&history=1`; the results are limited to that
  repository and ref.
- Step 3: "Code options" opens on its own with the values filled.

#### SRCH-006 · Choose places · P1
As: owner

1. With all ten ticked, untick "Docs".
2. Untick every place one by one down to the last, and untick that one too.
3. Press **Everything**.
4. Untick "Docs" again and reload `/search` with no query.

Expect:
- Step 1: the address holds `in=` with the nine others; the Docs section is gone.
- Step 2: the last place stays ticked.
- Step 3: all ten are ticked and `in` leaves the address.
- Step 4: the choice is remembered (`localStorage["lake-search-places"]`).

#### SRCH-007 · Nothing matches · P1
As: owner

1. Search for `zzqqxx`.

Expect:
- "0 results for “zzqqxx”", and every section reads "No matches."

#### SRCH-008 · More than 50 hits · P2
As: owner · Needs: a place with over 50 hits for one word (e.g. a common word in code)

1. Search for that word.

Expect:
- The section lists 50 hits and then "<n> more; narrow the search to see them."

#### SRCH-009 · The old code-search address · P2
As: owner · Needs: TD-REPO

1. Open `/search?query=<a word from README.md>`.

Expect:
- Only "Code" is ticked and the search runs with that word.

#### SRCH-010 · A failed place does not hide the others · P2
As: owner

1. In the Network panel, block `list_repositories`, then search `qa-`.
2. Press **Retry** in the Repositories section after unblocking.

Expect:
- Step 1: the Repositories section shows "Couldn't load repositories" with Lake's or the network's
  message and **Retry**; other sections answer as usual.
- Step 2: the section fills in.

#### SRCH-011 · A workspace that could not be searched · P2
As: owner · Needs: a workspace that is not Ready (e.g. right after WS-008)

1. Search `qa_` with Datasets ticked.

Expect:
- Hits from the Ready workspaces, and under the section "Could not search <workspace>: <message>".
- With no hit: "No matches in the workspaces that answered."

#### SRCH-012 · A place this Lake does not offer · P2
As: owner · Needs: a Lake that answers `unknown_capability` or `not_supported` for a place

1. Search with that place ticked.

Expect:
- Its section reads "Searching <place> is not available on this Lake yet."

## By persona

#### SRCH-013 · One workspace · P1
As: newcomer · Needs: WS-003

1. Open **Data → Search** and search `qa-`.

Expect:
- The sentence reads "Datasets, transcripts, projects, solutions, agents, models and drives are
  searched in the workspace **<name>**."
- Workspace hits carry no workspace name.

#### SRCH-014 · No workspace · P2
As: a persona with no workspace (newcomer before WS-003, or a fresh `local` account)

1. Open **Data → Search** and search `qa-`.

Expect:
- "Create a workspace to search its datasets, transcripts, projects, solutions, agents, models and
  drives."
- Each workspace section reads "Create a workspace to search <place>." (e.g. "Create a workspace to
  search datasets."); Repositories, Code and Docs still answer.

#### SRCH-015 · A member sees only what they may read · P1
As: member · Needs: TD-REPO (member has `read`)

1. Search `qa-`.

Expect:
- `qa-<MMDD>-repo` is listed; repositories and workspace objects the member cannot read are not.

#### SRCH-016 · A visitor is sent to sign in · P2
As: visitor

1. Open `/search?q=qa-`.

Expect:
- The browser is on `/sign-in?next=%2Fsearch%3Fq%3Dqa-`.

## Phone width

#### SRCH-017 · Search on a phone · P2
As: owner

1. At 390 px, open **Data → Search** and search `qa-`.

Expect:
- The box fills the width with **Search** beside or under it; the place chips wrap; long code
  lines wrap inside their hit.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/search/SearchPage.tsx:76-78` | Unticking the last ticked place keeps it on without saying why | SRCH-006 |
