# Data: Drives and Sharing

This file covers **Data → Drives** and every drive kind in the file manager: workspace files,
repository, virtual and storage. It also covers the **Storage** tab, and a repository's own Drive
pages: the **Files** tab, Preview, History, Compare Versions and Conflict. Then sharing a folder
(people, organizations, links), a link's page, **Shared with Me**, and the public `/share/<token>`
pages a visitor opens without signing in.

## Before you start

- Personas: `owner`, `member`, `newcomer`, `visitor`, `operator`.
- Test data: `TD-ORG`, `TD-ORGWS`, `TD-W2`, `TD-REPO`, `TD-FILES`, `TD-BIG`, `TD-CSV`, `TD-CSV-ODD`,
  `TD-STORAGE`.
- Data this file alone needs:
  - `DRV-MOUNT`: a service in `W1` that mounts `TD-STORAGE`'s volume (**Ship → Services → New
    service**, a SQLite service with that volume under **Storage**, [13-ship.md](13-ship.md)).
  - `DRV-CONFLICT`: a second text conflict on `TD-REPO`, made as `TD-CONFLICT` was (one file edited
    offline twice, both pushed). REPO-069 used up `TD-CONFLICT`.
  - `DRV-SUB`: in `qa-files/`, a folder `shared/` holding `hello.txt`, `notes.md`, an empty
    `empty.txt`, a small binary `blob.bin`, and a folder `inner/` with `deep.csv`.
- Cases DRV-002 to DRV-006 make the drives the rest of the file uses:
  - `qa-<MMDD>-files` (workspace files)
  - `qa-<MMDD>-repodrive` (repository, a new repository `qa-<MMDD>-repodrive`)
  - `qa-<MMDD>-repolink` (repository, using `TD-REPO`)
  - `qa-<MMDD>-links` (virtual)
- DRV-080 makes `qa-<MMDD>-orgrepo` in `TD-ORGWS`, so that `member` (a member of `TD-ORG`) can be
  named in a share.
- The Drives pages are workspace pages under `/w/<W1 id>/…`. Keep the top bar's workspace on `W1`
  unless a case says otherwise. The repository Drive pages (`/drive…`) and the share pages are not
  workspace pages.
- Every case that uploads uses `qa-files/`. Keep the Network panel open: uploads are
  `/v1/upload_drive_entries`, `/v1/begin_drive_upload`, `/v1/append_drive_upload`,
  `/v1/finish_drive_upload` or `/v1/upload_drive_files`.

## Drives page

#### DRV-001 · A workspace with no drive yet · P2
As: owner · Needs: TD-W2

1. Switch the workspace to `W2` (only if it has no drive). Open **Data → Drives**.

Expect:
- Eyebrow "Data", heading "Drives", sub "Where this workspace keeps its files: repositories with
  history, workspace files, folders of shortcuts, and its buckets, filesystems and volumes."
- The tabs are **Drives** (with a count) and **Storage**. The header has **New drive**.
- The four figures **Drives**, **Workspace files**, **Repositories** and **Virtual** all read 0.
- "No drives yet" / "A drive holds files and folders. Keep them as workspace files, in a repository
  with history, or as shortcuts to what you already have." with **New drive**.
- **Recent Files** reads "Nothing changed yet" / "Files you upload to a workspace-files drive show here."

#### DRV-002 · Create a workspace-files drive · P0
As: owner

1. On `W1`, open **Data → Drives** and press **New drive**.
2. Type `qa-<MMDD>-files`. Keep **Workspace files** chosen.
3. Press **Create drive**.

Expect:
- The drawer is titled "New Drive", subtitle "A place for this workspace's files".
- **Name** is focused, with the placeholder "Team files".
- The fieldset "Keeps its files as" has four radio cards:
  - **Workspace files**, "Files and folders kept in this workspace, encrypted. Moving or renaming
    never copies them. No version history."
  - **Repository**, "A repository with history: every upload, move and delete is a change you can
    compare and restore. Shareable by folder."
  - **Virtual**, "Folders of shortcuts to things that live elsewhere: repositories, files,
    datasets, transcripts, projects, agents, models and links."
  - **Storage**, "One of this workspace's buckets or filesystems, opened as a drive. Nothing is
    copied; deleting the drive never deletes the storage."
- A toast reads "Drive “qa-<MMDD>-files” created". The browser is on `/w/<W1>/drives/drive?id=<id>`.
- The eyebrow reads "Drive · Workspace files" and the heading is the drive name.

#### DRV-003 · A drive name is checked before it is sent · P1
As: owner

1. Press **New drive**. Type `a/b`.
2. Replace it with `..`.
3. Clear it.

Expect:
- Steps 1 and 2: under **Name**, "A name has no slash or backslash and is at most 255 bytes."
  **Create drive** is disabled.
- Step 3: **Create drive** is disabled with no message.

#### DRV-004 · Create a repository drive with a new repository · P1
As: owner

1. Press **New drive**. Type `QA <MMDD> Repo Drive`. Choose **Repository**.
2. Read **Repository name**. Then rename the drive to `qa-<MMDD>-repodrive`.
3. Change **Repository name** to `qa-<MMDD>-repodrive` and press **Create drive**.

Expect:
- Under **Repository**, **Create a new one** is chosen. **Use one of this workspace** is enabled
  when `W1` has a repository.
- **Repository name** has the help "Lowercase letters, numbers and hyphens." It follows the drive
  name until edited: step 1 shows `qa-<mmdd>-repo-drive`.
- After an edit, changing the drive name no longer changes it.
- The drive opens. Its eyebrow reads "Drive · Repository".
- **Data → Repositories** now lists `qa-<MMDD>-repodrive`. It was created with its first change, so
  its home page has a `main` reference (confirm).

#### DRV-005 · Create a repository drive over an existing repository · P1
As: owner · Needs: TD-REPO

1. Press **New drive**. Type `qa-<MMDD>-repolink`. Choose **Repository**, then **Use one of this workspace**.
2. Choose `TD-REPO` under **Repository** and press **Create drive**.

Expect:
- The select starts at "Choose a repository…". Its help reads "Deleting the drive later never
  deletes the repository."
- **Create drive** is disabled until a repository is chosen.
- The drive opens and lists `TD-REPO`'s top-level folders and files.

#### DRV-006 · Create a virtual drive · P1
As: owner

1. Press **New drive**. Type `qa-<MMDD>-links`. Choose **Virtual**. Press **Create drive**.

Expect:
- The eyebrow reads "Drive · Virtual". The sub is the Virtual detail sentence.
- The header has **New shortcut** and **New folder**, and no **Upload**.
- The figures are **Folders** and **Shortcuts** only. There is no Files or History figure.
- The empty folder reads "This folder is empty" / "Add folders and shortcuts to what this workspace
  already has." with **New folder** and **New shortcut**.

#### DRV-007 · A taken drive name is refused · P2
As: owner

1. Press **New drive**. Type `qa-<MMDD>-files` again and press **Create drive**.

Expect:
- The drawer stays open. Lake's message shows under **Name** (confirm the text).

#### DRV-008 · Filter the drives and their kinds · P1
As: owner

1. Back on **Data → Drives**, read the figures and cards.
2. Press the **Repository** chip.
3. Type `zzzz` in **Filter drives by name**.
4. Clear the text, keep the **Repository** chip, and press **Virtual** instead.
5. Press **Clear the filters** on the empty state.

Expect:
- **Drives** counts 4. **Workspace files** 1, **Repositories** 2, **Virtual** 1.
- The chips are **All**, **Files**, **Repository**, **Virtual** and **Storage**, each with a count.
- Each card shows its kind label, the name, a kind icon with its tagline and "created <relative>".
  The taglines are:
  - "Encrypted files in this workspace".
  - "Files with history and sharing".
  - "Shortcuts to what you already have".
  - "A bucket or filesystem, browsed in place".
- Step 2 shows the two repository drives.
- Step 3: "Nothing matches" / "No drive is named like “zzzz” among repository drives." with **Clear
  the filters**.
- Step 4 (no virtual drive matching): a chip-only miss reads "No drive is named like “” among …"
  (see defect 10).
- Step 5 clears both, and every card shows again.

## Files in a workspace-files drive

#### DRV-009 · An empty workspace-files drive · P2
As: owner

1. Open `qa-<MMDD>-files`.

Expect:
- Figures:
  - **Folders** 0.
  - **Files** 0, with the sub "0 B".
  - **Shortcuts** 0, "all reachable".
  - **History** "None", "a deleted file is gone".
- The header has **New shortcut**, **New folder**, **Upload** (primary) and a **⋯** menu named
  "Drive actions". The menu holds **Upload a folder** and **Drive settings**.
- The empty folder reads "This folder is empty" / "Drop files or folders here, or upload them."
  with **New folder**, **New shortcut** and **Upload files**.
- The search box reads "Search this drive…". **Sort** offers `name`, `size` and `modified`.

#### DRV-010 · Upload files with progress · P0
As: owner · Needs: TD-FILES

1. Press **Upload**. Choose every file of `TD-FILES` (not the folder).

Expect:
- An info banner "Uploading <file>" shows a meter "<sent> of <total>" while it runs.
- A toast reads "<n> files uploaded".
- Each file is listed with **Kind** `file`, its size and "Modified" time.
- The **Files** figure counts them, with their total size as its sub.
- **Data → Drives → Recent Files** lists them with the drive's name as a chip.

#### DRV-011 · Upload a folder from the menu · P1
As: owner · Needs: TD-FILES

1. Open **⋯** → **Upload a folder**. Choose the folder tree from `TD-FILES`.

Expect:
- The browser's folder chooser opens (not a file chooser).
- The folder and its subfolders are created. Each file keeps its place inside them.
- Open the subfolders: the path bar reads `<drive> › <folder> › <subfolder>`. Each crumb but the
  last is a link.

#### DRV-012 · Drop files and a folder onto the list · P1
As: owner · Needs: DRV-SUB

1. Drag `DRV-SUB`'s `shared/` folder from the desktop onto the folder list.
2. Drag `TD-CSV` from the desktop onto the `shared` folder's row.

Expect:
- While dragging over the list, it is outlined. While dragging over the folder row, that row is outlined.
- Step 1 makes `shared/` with `inner/deep.csv` inside, walking the folder.
- Step 2 puts `accounts.csv` inside `shared/`, not in the current folder.

#### DRV-013 · Large files go up in pieces · P1
As: owner · Needs: TD-BIG

1. Upload the 5 MiB, 9 MiB and 17 MiB files together.

Expect:
- In the Network panel, each of the three files calls `begin_drive_upload`, several
  `append_drive_upload` (8 MiB at most each) and `finish_drive_upload`. They are all over 4 MiB.
- The progress meter moves during each file.
- All three are listed with their sizes. Each downloads byte for byte identical (`cmp`).

#### DRV-014 · Uploading a name that is taken · P2
As: owner · Needs: TD-FILES

1. Upload `TD-FILES`'s `.txt` file again into the same folder.

Expect:
- The second copy gets the next free name, such as `<name> (2).txt` (confirm). The first is unchanged.

#### DRV-015 · New folder, and its name rules · P1
As: owner

1. Press **New folder**. Type `x/y`.
2. Type `qa-folder` and press **Create folder**.
3. Press **New folder** again, type `qa-folder`, and press **Create folder**.

Expect:
- The modal is titled "New Folder" with **Name** focused.
- Step 1: "A name has no slash or backslash and is at most 255 bytes." **Create folder** is disabled.
- Step 2: a toast reads "Folder created" and the folder is listed first (folders before files).
- Step 3: Lake's refusal shows in the modal (confirm the text).

#### DRV-016 · Open, view and download a workspace file · P1
As: owner · Needs: TD-FILES

1. Press the `.png` file's name. Then the `.pdf`'s.
2. Press the `.json` file's name.
3. Open the `.csv` file's **⋯** menu (named "Actions for <name>") and press **Download**.

Expect:
- Step 1: each opens in a new tab at `/drive/files/download?…&inline=1`, shown by the browser.
- Step 2: the `.json` file (not a type shown inline) downloads instead of opening.
- A file shown inline also has **View** in its menu. Every file has **Download**.
- Step 3 saves the file under its name, identical to the original.

#### DRV-017 · Select items by click, Shift and Ctrl · P1
As: owner · Needs: TD-FILES

1. Press a row (not its name). Then Shift-press a row three below.
2. Ctrl-press (⌘ on macOS) one of the selected rows.
3. Tick the header box named "Select every item". Untick it.
4. Select two rows and press **Clear**.

Expect:
- Step 1: four rows are tinted. The selection bar reads "4 items selected" with **Move**,
  **Delete** and **Clear**.
- Step 2 removes that one row: "3 items selected".
- Step 3 selects every row shown, then none.
- A row's own box ("Select <name>") toggles just that row.
- Selected rows have `aria-selected="true"`.

#### DRV-018 · Drag items onto a folder and onto a crumb · P1
As: owner · Needs: TD-FILES, DRV-015

1. Select two files. Drag them onto the `qa-folder` row.
2. Open `qa-folder`. Drag one file onto the drive name in the path bar.
3. Drag `qa-folder` onto itself.

Expect:
- Step 1: a toast reads "Moved 2 items." Both leave this folder and appear in `qa-folder`.
- Step 2: "Moved 1 item." The file is back at the top.
- Step 3: a toast reads "Nothing moved." An internal drag never starts an upload.

#### DRV-019 · Move with the folder picker · P1
As: owner · Needs: DRV-015

1. On a file, open **⋯** → **Move**.
2. Press `qa-folder`, then **Move here**.
3. Open **Move** on `qa-folder` itself.

Expect:
- The dialog is titled "Move <name>", subtitle "Open the folder to move into, then choose it.", and
  has a search box "Search <drive name>…".
- Folder rows open on a press. The crumbs show where the picker is.
- An empty folder reads "No folders here" / "Move here puts it in this folder."
- Step 2: a toast reads "Moved 1 item."
- Step 3: the `qa-folder` row is disabled, and **Move here** is disabled while inside it.
- Moving a file into the folder it is already in: record the toast. A workspace-files drive sends
  the move to Lake, which decides (confirm).

#### DRV-020 · Rename an item · P1
As: owner · Needs: TD-FILES

1. On a file, open **⋯** → **Rename**. Look at **Rename** without changing the name.
2. Type a new name with a `/`.
3. Type `qa-renamed.txt` and press **Rename**.

Expect:
- The modal is titled "Rename <name>". **New name** holds the current name.
- Step 1: **Rename** is disabled while the name is unchanged.
- Step 2: "A name has no slash or backslash and is at most 255 bytes."
- Step 3: a toast reads "Renamed" and the row shows the new name.

#### DRV-021 · Search this drive · P1
As: owner · Needs: DRV-012

1. At the top of the drive, type `deep` in **Search this drive…**.
2. Press the match's name, then go back. Press the line under it.
3. Type `zzzz`.

Expect:
- At once, the folder's own rows are filtered. Nothing here is named `deep`, so it reads
  "Nothing in This Folder Matches" / "No item here is named like “deep”. Matches elsewhere in the
  drive are listed below."
- About a quarter second after typing stops, **Elsewhere in This Drive** lists `deep.csv` with "deep"
  marked, the line "qa-<MMDD>-files / shared / inner", and its size and time.
- The header count shows "…" while waiting.
- Step 2: the name opens the file in a new tab (`text/csv` is shown inline). The line under it
  opens `inner/`.
- Step 3: "No other folder of this drive holds a match."

#### DRV-022 · Sort the folder · P2
As: owner · Needs: TD-FILES

1. Choose **Sort** `size`. Press the direction button (named "Ascending; sort descending").
2. Choose `modified`.

Expect:
- Folders stay above files in every order.
- The direction button's name changes to "Descending; sort ascending".
- Sorting is not in the address. A reload returns to name, ascending (see defect 33).

#### DRV-023 · Load a CSV as a dataset · P1
As: owner · Needs: TD-CSV

1. On `accounts.csv`, open **⋯** → **Load as dataset**.
2. Clear **Dataset name** and type `1bad`.
3. Type `qa_<MMDD>_from_drive`, tick "If the name is taken, save it under the next free name", and
   press **Load dataset**.

Expect:
- The modal is titled "Load accounts.csv as a Dataset", subtitle "The file's rows become a dataset
  of this workspace, for SQL and training."
- **Dataset name** starts as `accounts`. Its help reads "Letters, digits and underscores, starting
  with a letter, so SQL can query it."
- A quiet banner reads "A CSV of at most 16 MiB. A CSV row whose field count differs from the
  header is refused with its line number."
- Step 2: **Load dataset** is disabled.
- Step 3 opens `/w/<W1>/train/data?…loaded=qa_<MMDD>_from_drive…` with the loaded banner
  ([08-data-datasets.md](08-data-datasets.md)).
- **Load as dataset** is only in the menu of `.csv`, `.jsonl` and `.ndjson` files.

#### DRV-024 · Dataset names from odd file names · P2
As: owner · Needs: TD-CSV-ODD

1. Upload `My Data 2024.csv`, `order.csv`, `bad.csv` and `events.ndjson`.
2. Open **Load as dataset** on each, reading the suggested name. Load `bad.csv`.

Expect:
- The suggestions are `my_data_2024`, `order_data` and `events`. The `.ndjson` banner reads "A JSONL file of at most 16 MiB. …".
- Loading `bad.csv` is refused inside the modal with Lake's message naming line 3 (confirm).

#### DRV-025 · Delete one file and a folder · P1
As: owner · Needs: DRV-015

1. On one file, open **⋯** → **Delete**. Press **Cancel**.
2. Open it again and press **Delete**.
3. Select a file and `qa-folder`. Press **Delete** in the selection bar.

Expect:
- The modal is titled "Delete “<name>”?". It reads "Workspace files have no history: this cannot be
  undone." It has **Cancel** and **Delete** (danger). There is no text to type.
- Step 2: a toast reads "Deleted" and the row is gone.
- Step 3: the title is "Delete 2 items?". The text adds " A folder goes with everything in it." A
  toast reads "2 items deleted".

#### DRV-026 · Unfinished uploads · P2
As: owner · Needs: TD-BIG

1. Start uploading the 17 MiB file. While the meter shows, close the tab.
2. Open the drive again in a new tab.
3. Press **Remove** on the banner.

Expect:
- Step 2: a quiet banner "1 Unfinished Upload" reads "<name> (<n> min ago). An upload not finished
  holds its name until it is removed, or until 24 hours after it began."
- Step 3: a toast reads "Unfinished upload removed" and the banner goes away.
- If **Remove** fails, nothing says so (see defect 7). Record it.

#### DRV-027 · A reader cannot change a workspace drive · P1
As: member · Needs: DRV-002

1. As `member` on `W1`, open `qa-<MMDD>-files`.
2. Press **Upload** and choose a file.
3. Press **New folder**, type `qa-member`, and press **Create folder**.

Expect:
- The drive lists the same items.
- Step 2: a red banner "Upload stopped" shows Lake's refusal, with **Dismiss**.
- Step 3: the refusal shows inside the modal. Nothing is created.

## Shortcuts and the virtual drive

#### DRV-028 · A shortcut to a repository · P1
As: owner · Needs: TD-REPO, DRV-006

1. Open `qa-<MMDD>-links` and press **New shortcut**.
2. Keep **Opens** as **Repository**. Choose `TD-REPO`.
3. Click into **Name** and type `qa repo shortcut`.
4. Press **Create shortcut**. Press the shortcut's name.

Expect:
- The modal is titled "New Shortcut", subtitle "A link to something that lives elsewhere. Deleting
  the shortcut never deletes what it opens."
- The **Opens** help reads "A repository of this workspace."
- Step 2 fills **Name** with the repository's name.
- Step 3: the click selects the suggestion, so typing replaces it rather than adding to it.
- **Create shortcut** is disabled until a target and a name are set.
- A toast reads "Shortcut created". The row's **Kind** reads "Repository" with a link icon.
- Step 4 opens `/repository?id=<TD-REPO id>`.

#### DRV-029 · A shortcut to a web address, checked as it is typed · P1
As: owner · Needs: DRV-006

1. Press **New shortcut**. Choose **Opens** → **Web address**.
2. Type `example.com`, then `https://example.com`. Name it `qa-site`. Create it.
3. Press its name.

Expect:
- Step 2: with `example.com` a muted line reads "A web address starts with http:// or https://."
  and **Create shortcut** is disabled. The URL with `https://` clears both.
- Step 3 opens `https://example.com` in a new tab. The row shows a small external-link icon.

#### DRV-030 · A shortcut to an item of another drive · P1
As: owner · Needs: DRV-012

1. Press **New shortcut**. Choose **Opens** → **Item in another drive**.
2. Choose `qa-<MMDD>-files` under **Drive**.
3. In the picker, press `shared` once. Then double-press it, and press `hello.txt`. Press **Choose**.
4. Create the shortcut and press its name.

Expect:
- The **Drive** select offers no repository drive. Step 2 opens the picker at once, titled
  "Choose an Item in qa-<MMDD>-files", subtitle "Open folders to find the item, or search for it,
  then choose it."
- Step 3: a single press on `shared` chooses it ("Chosen: qa-<MMDD>-files / shared"), and a double
  press opens it. Then `hello.txt` is chosen, with a check named "Chosen".
- Back in the dialog, the **Drive** help reads "Chosen: qa-<MMDD>-files / shared/hello.txt" (or the
  item's path). The button reads **Choose another item**.
- Step 4 opens `qa-<MMDD>-files` at `shared/` with `hello.txt` selected (`focus` in the address,
  then replaced).

#### DRV-031 · Shortcuts to a repository path, a dataset and a project · P2
As: owner · Needs: TD-REPO

1. Make a **File or folder in a repository** shortcut: `TD-REPO`, path `README.md`.
2. Make a **Dataset** shortcut `datasets/qa_<MMDD>_from_drive`, and a **Project** shortcut with any name.
3. Press each name.

Expect:
- Step 1: the path's last part becomes the suggested name. The help reads "From the repository's
  top, such as reports/2026/q3.pdf." It opens `/drive/preview?repository_id=…&ref=main&path=README.md`.
- The dataset opens `/w/<W1>/train/dataset?name=qa_<MMDD>_from_drive&database=datasets`.
- The project opens `/w/<W1>/design/project?name=…`. Lake does not check datasets or projects.
- An empty required field shows its line: "Name the path in the repository.", "Name what the
  shortcut opens." or "Name the project.".

#### DRV-032 · A broken shortcut · P2
As: owner · Needs: DRV-030

1. In `qa-<MMDD>-files`, delete `shared/hello.txt`.
2. Open `qa-<MMDD>-links`.
3. Delete the broken shortcut.

Expect:
- Step 2: the shortcut's name is not a link. A red **Broken** chip has the reason as its tooltip
  (confirm). **Shortcuts** reads "1 broken".
- Step 3: the modal reads "Only the shortcut goes; what it points at stays where it is."

## Repository drives in Drives

#### DRV-033 · A repository drive keeps history · P1
As: owner · Needs: DRV-004, TD-FILES

1. Open `qa-<MMDD>-repodrive`. Upload `TD-FILES`'s `.md` and `.csv`.
2. Open **⋯** and read the menu.
3. Press the `.md` file's name.

Expect:
- The header has **New folder** and **Upload**, and no **New shortcut**.
- **History** reads "Kept", "every change can be restored".
- The menu holds:
  - **Upload a folder**.
  - **History**: opens `/drive/history?…`.
  - **Share this folder**: opens `/drive/share?…`.
  - **Open the repository**: opens `/repository?id=…`.
  - **Drive settings**.
- Each file's menu has **History**.
- Step 3 opens the repository preview `/drive/preview?…` in the same tab.
- The delete modal reads "This writes a new change without it. Earlier versions stay in the
  repository's history and can be restored."

#### DRV-034 · A file too large for one request in a repository drive · P2
As: owner · Needs: TD-BIG

1. Only if `TD-BIG` has a single file over 64 MiB once encoded: upload it into `qa-<MMDD>-repodrive`.

Expect:
- A banner "Some files were not uploaded" names "<path> (<size>: larger than one request can
  carry)", with **Dismiss**.
- Otherwise mark `blocked`. The 17 MiB file uploads.

## Drive settings

#### DRV-035 · Rename a drive · P1
As: owner · Needs: DRV-006

1. In `qa-<MMDD>-links`, open **⋯** → **Drive settings**.
2. Look at **Rename drive**. Change the name to `qa-<MMDD>-links2` and press it.

Expect:
- The modal is titled "Drive Settings", subtitle "qa-<MMDD>-links · Virtual".
- **Rename drive** is disabled until the name changes.
- A toast reads "Drive renamed". The heading and the Drives card show the new name.
- **Delete This Drive** reads "Its folders and shortcuts go; what they point at stays where it is."

## Storage tab

#### DRV-036 · The Storage tab · P0
As: owner · Needs: TD-STORAGE, DRV-MOUNT

1. On **Data → Drives**, press **Storage**.

Expect:
- The address gains `?tab=storage`. **New drive** leaves the header.
- Figures:
  - **Storage**: "buckets, filesystems, volumes", or "<n> changing".
  - **Opens as a drive**: "buckets and filesystems".
  - **Volumes**: "shown, not browsed".
  - **Size**: "<n> GiB", "asked for, in all".
- The card **Buckets, Filesystems and Volumes** has **New storage**. Its text ends with a **Usage** link.
- Columns: **Storage**, **Size**, **Mounted By**, **This Month**, **Status** and **Endpoint**,
  plus the actions.
- The volume row:
  - "Volume · <provider>" under its name, "<n> GiB" with "<x> used (<p>%)" or "measured once
    mounted" under it.
  - Under **Mounted By**: the `DRV-MOUNT` service as a link, then "service · /mnt/<name>".
  - A **This Month** link to Usage, the status `Ready`, and the chip **Shown, not browsed**.
  - **Show connection** and **Delete**.
- The bucket row shows "—" for **Size** and **This Month**, with **Open as a drive**,
  **Show connection** and **Delete**.

#### DRV-037 · Old links to Storage still open it · P2
As: owner

1. Open `/w/<W1>/storage`.
2. Open `/w/<W1>/drives#storage`.
3. Open the command palette (⌘K), type `Storage` and choose it.

Expect:
- Each lands on `/w/<W1>/drives?tab=storage` with the Storage tab current.

#### DRV-038 · Show a store's connection · P1
As: owner · Needs: TD-STORAGE

1. On the volume row, press **Show connection**.
2. Press **Escape**.

Expect:
- The modal is titled "Connection to <name>", subtitle "What a service in your workspace mounts or
  calls to use it. It is not shown again unless you ask." It lists each key and value.
- While it loads, record whether every row's **Show connection** spins (see defect 8).
- Do not screenshot a secret value.

#### DRV-039 · New storage: a volume · P1
As: owner

1. Press **New storage**. Read **Type and provider**.
2. Choose the volume type. Type `Bad_Name` as **Name** and press **Create storage**.
3. Type `qa-<MMDD>-vol`, keep **Size (GiB)** 10, and press **Create storage**.

Expect:
- The modal is titled "New Storage". **Type and provider** lists only the available providers. It
  has the help "Shared network filesystems are coming later." when some are not available.
- A volume shows **Size (GiB)** (1 to 100) next to **Name**.
- Step 2: the browser refuses the name (pattern `[a-z][a-z0-9-]{0,30}`).
- Step 3: a toast reads "Creating qa-<MMDD>-vol". The row shows "Waiting for the operator" or
  "Creating", then `Ready`.
- While any row is changing, a quiet banner reads "This list checks again every 3 seconds while
  storage is changing."

#### DRV-040 · New storage: your own bucket's fields · P2
As: owner · Needs: TD-SECRET

1. Press **New storage**. Choose each of the bucket providers in turn: Cloudflare R2, AWS S3,
   Backblaze B2, Google Cloud Storage, Azure Blob.
2. With R2, leave **Endpoint** empty and press **Create storage**.
3. Press **Cancel**.

Expect:
- Each shows **Bucket** (**Container** for Azure), **Folder in it** (help "Optional: the storage
  covers only this folder."), **Endpoint**, **Region** (not for Azure), **Secret mount** and **Secret**.
- The explainer reads "Your own bucket, opened with a secret of this workspace that holds
  access_key_id and secret_access_key. …". For Azure it reads "Your own container" and
  "account_key, or sas_token".
- **Endpoint** placeholders, with **Endpoint** required for R2 and Azure only:
  - R2: `https://<account id>.r2.cloudflarestorage.com`, help "Your account's S3 API endpoint."
  - Azure: `https://<account>.blob.core.windows.net`.
  - B2: `https://s3.<region>.backblazeb2.com`, "Or leave it empty and give the region."
  - GCS: `https://storage.googleapis.com`, "Empty: Google Cloud Storage's XML API."
  - S3: `https://s3.<region>.amazonaws.com`, "Empty: AWS S3 in the region below."
- **Region** placeholders: `us-east-1` for S3, `us-west-004` for B2, `auto` otherwise.
- **Secret mount** starts at "Choose a mount" and lists `TD-SECRET`'s mount.
- Step 2: the browser refuses the empty required endpoint.
- Nothing is created.

#### DRV-041 · Open a Ready bucket as a drive · P1
As: owner · Needs: TD-STORAGE

1. On the bucket row, press **Open as a drive**.
2. Press **Create drive**.
3. Go back to the Storage tab.

Expect:
- The New Drive drawer opens with **Storage** chosen, the store selected under **Storage** and its
  name as **Name**.
- The **Storage** help reads "Only a bucket or filesystem opens as a drive; a volume belongs to the
  service that mounts it. Nothing is copied, and deleting the drive never deletes the storage."
- The volume is listed but disabled, with " (volume: shown, not browsed)".
- The drive opens with the eyebrow "Drive · Storage".
- Step 3: the bucket row now offers **Open drive** instead of **Open as a drive**.
- **Data → Drives** counts it under the **Storage** chip.

#### DRV-042 · Work in a storage drive · P1
As: owner · Needs: DRV-041, TD-FILES, TD-BIG

1. In the storage drive, upload the `.txt` file and the 17 MiB file together.
2. Delete the `.txt` file.

Expect:
- The header has no **New shortcut**. **History** reads "None".
- Step 1: the `.txt` uploads. A banner "Some files were not uploaded" names "<path> (17.0 MiB: a
  storage drive takes files of up to 16 MiB)".
- Step 2: the modal reads "This deletes it from the storage itself, which keeps no history: this
  cannot be undone."
- **Drive settings** reads "The drive goes; the storage it opens stays, with every file in it.
  Delete the storage from Drives → Storage."

#### DRV-043 · Mounted storage cannot be deleted · P1
As: owner · Needs: DRV-MOUNT

1. On the mounted volume's row, press **Delete**.
2. Type the volume's exact name.

Expect:
- The modal is titled "Delete <name>?". A red banner reads "The service <link> mounts it. Remove
  the mount first, in the service's or app's storage."
- **Delete <name>** stays disabled even with the right name typed.

#### DRV-044 · Delete storage, wrong name then right · P1
As: owner · Needs: DRV-039

1. On `qa-<MMDD>-vol`, press **Delete**.
2. Type `qa-<MMDD>-VOL`.
3. Type `qa-<MMDD>-vol` and press **Delete qa-<MMDD>-vol**.

Expect:
- A red banner reads "Deleting storage deletes the data in it, which cannot be recovered. A drive
  that opens it stops working; delete that drive too."
- The field is labelled "Type `qa-<MMDD>-vol` to delete it".
- Step 2: the button stays disabled (the match is exact, case and all).
- Step 3: a toast reads "Deleting qa-<MMDD>-vol". The row reads `Deleting` with no actions, then leaves.
- For your own bucket the banner instead reads "Lake forgets this bucket. The bucket and what is in
  it stay in your account. A drive that opens it stops working; delete that drive too." (record it
  if one exists).

#### DRV-045 · A reader on the Storage tab · P2
As: member

1. On `W1`, open **Data → Drives → Storage**. Press **New storage**, fill a volume, and create it.

Expect:
- The list loads.
- The create is refused inside the modal with Lake's message. Nothing is created.

## A repository's Files tab

These cases use the repository `qa-<MMDD>-repodrive` from DRV-004, opened from **Data →
Repositories** → the repository → **Files**.

#### DRV-046 · The Files tab of a repository · P1
As: owner · Needs: DRV-033

1. Open the repository and press the **Files** tab.

Expect:
- The address is `/drive?repository_id=<id>`. The path bar reads the repository name with a drive icon.
- The heading is the repository name. **Files** is the current tab.
- The sub reads "Folder `/` at `main`. Moves and deletes create new history; prior versions remain
  available."
- The header has **Share**, **New folder**, **Upload** and the **⋯** "Folder actions" menu
  (**History**, **Conflicts**).
- Figures **Folders**, **Files** ("in this folder") and **Size** ("of these files").
- The toolbar has **Filter by name…**, **Sort**, the direction button and the **List** / **Grid**
  toggle.
- A dashed drop zone reads "Choose files or drop them here".
- The table's columns are **Name**, **Kind** (`folder` or `file`), **Size** and **Modified**, plus each row's menu.

#### DRV-047 · Upload into a repository folder · P0
As: owner · Needs: TD-FILES

1. Press the drop zone and choose the `.txt` file.
2. Drag the `.json` and `.png` files from the desktop onto the drop zone.

Expect:
- While uploading, the zone reads "Uploading…" and **Upload** spins.
- Step 1: a toast reads "File uploaded". Step 2: "2 files uploaded". A drag over the zone colours it.
- Each upload is one `upload_drive_files` request and one new change. The **Changes** tab lists them.
- The rows show the new files.

#### DRV-048 · A selection too large for one request · P1
As: owner · Needs: TD-BIG

1. Press **Upload** and choose the selection over 64 MiB.

Expect:
- A red banner "Upload refused" reads "These files are too large to upload at once: <size> encoded,
  of at most 64.0 MiB." Nothing is sent.
- Record whether the tab freezes before the banner shows (see defect 11).

#### DRV-049 · List, grid and sort stay in the address · P1
As: owner · Needs: DRV-047

1. Press **Grid**. Choose **Sort** `size`. Press the direction button.
2. Reload.
3. Type `zzzz` in **Filter by name…**.

Expect:
- The address carries `view=grid`, `sort=size` and `direction=desc`. After the reload they are kept.
- Grid shows a card per item with its icon, menu, name, and "<folder or size> · <modified>".
- Step 3: "Nothing matches" / "No item here is named like “zzzz”." The filter is not in the address.

#### DRV-050 · New folder, rename, move and delete in a repository · P1
As: owner · Needs: DRV-047

1. Press **New folder**, type `archive`, and press **Create folder**.
2. On the `.txt` file, open **⋯** → **Rename**, type `renamed.txt`, and press **Rename**.
3. On `renamed.txt`, open **Move**, type `/archive/`, and press **Move**.
4. In `archive`, open **Delete** on `renamed.txt` and press **Delete**.

Expect:
- Step 1: the modal "New Folder" has the subtitle "In /". A toast reads "Folder created".
- Step 2: the modal is "Rename <name>" with **New name**. A toast reads "Renamed". The folder reloads.
- Step 3: the modal "Move <name>" has **Destination folder** (placeholder `team/archive`, help "Leave
  empty to move it to the top of the repository."). Slashes around the folder are ignored. A toast
  reads "Moved" and the browser opens `archive`.
- Step 4: the modal "Delete renamed.txt?" reads "This writes a new change without it. Earlier
  versions stay in its history and can be restored." A toast reads "Deleted".
- An error in any of these shows in red inside its modal.

#### DRV-051 · An empty repository folder · P2
As: owner · Needs: DRV-050

1. Open `archive` after its file is gone.

Expect:
- "This folder is empty" / "Upload files or create a folder to start." with **New folder** and
  **Upload files**.

#### DRV-052 · A reader sees every control and is refused · P1
As: member · Needs: TD-REPO

1. Open `TD-REPO`'s **Files** tab. Upload a file. Then rename a file.

Expect:
- **Share**, **New folder**, **Upload** and the item menus all show (see defect 12).
- The upload shows "Upload refused" with Lake's message. The rename shows the refusal inside its
  modal. Nothing changes.

## Preview, History, Compare and Conflict

#### DRV-053 · Text previews · P0
As: owner · Needs: TD-FILES, DRV-033

1. In `qa-<MMDD>-repodrive`, upload the rest of `TD-FILES`.
2. Open the `.md`, `.txt`, `.csv` and `.json` files in turn.

Expect:
- Each opens `/drive/preview?repository_id=…&ref=main&path=…`. The heading is the file name. The sub
  has a format chip, the size and the path.
- The header has **Back to folder**, **History** and **Download** (primary).
- The side card **File** lists Format, Media type, Size, Digest and Ref.
- In **Preview**:
  - Markdown is rendered and raw HTML in it is escaped.
  - Text shows as text.
  - CSV shows as a table.
  - JSON shows formatted.
- **Download** saves the exact bytes (`cmp`).

#### DRV-054 · Image, PDF, video and audio previews · P1
As: owner · Needs: DRV-053

1. Open the `.png`, `.jpg`, `.pdf`, `.mp4`, `.mp3` and `.wav` files.

Expect:
- The images show. The PDF shows in the browser's viewer, or "This browser cannot display the PDF.".
- Video and audio show native controls and play.

#### DRV-055 · Files with no preview · P1
As: owner · Needs: DRV-053

1. Open the `.svg` and `.bin` files.

Expect:
- The format chip reads `download`. Media type is `application/octet-stream`. The preview reads
  "No preview is available."
- **Download** still saves the file.

#### DRV-056 · History after uploading a file again · P0
As: owner · Needs: DRV-053

1. Change `TD-FILES`'s `.txt` locally and upload it again into the same folder.
2. On the file, open **⋯** → **History**.

Expect:
- The heading reads "History of <name>". The sub reads "`<path>`. Restoring writes a new change and
  does not rewrite prior versions." **Back to folder** is there.
- Figures: **Changes** ("touch this path"), **Versions** ("restorable") and **Latest**.
- Each row reads "modified" or "added". The upload over the old file reads **modified**, not
  deleted. The previous version is still listed.
- With two versions, a quiet banner reads "Pick a version to compare **from** and one to compare
  **to**, then compare them line by line." There is **Compare selected text versions**.
- **From** defaults to the second row and **To** to the first.
- Each version row has **Restore**.

#### DRV-057 · Compare two text versions · P1
As: owner · Needs: DRV-056

1. Press **Compare selected text versions**.
2. Press **Back to history**.

Expect:
- The address is `/drive/history/diff?…&left=<change>&right=<change>`.
- The heading reads "Compare <name>". The sub reads "Two preserved text versions. Neither version is
  changed by this comparison."
- Cards **From** and **To** list Change, Size and Digest, with an arrow between them on a wide screen.
- **Line by line** shows "+a −r". Removed lines start with `-`, added lines with `+`, and unchanged
  lines with a space.

#### DRV-058 · Restore an older version · P1
As: owner · Needs: DRV-056

1. On the older row, press **Restore**. Press **Escape**.
2. Press **Restore** again, then **Restore** in the dialog.

Expect:
- The dialog is titled "Restore this version?". It reads "Writes a new change that puts `<path>`
  back as it was in <id>. No earlier version is rewritten."
- Step 1 closes it with nothing done.
- Step 2: a toast reads "Version restored". A new row appears at the top; the older rows stay.
- The preview shows the old content again.

#### DRV-059 · A deleted file in its history · P2
As: owner · Needs: DRV-056

1. Delete the file. Open `/drive/history?…path=<that path>`.

Expect:
- The latest row has a red **deleted** chip, "—" under **From** and **To**, and "Deleted" instead
  of **Restore**.
- **Versions** has the sub "1 deletions" (sic).

#### DRV-060 · Compare a version with itself, and binary versions · P2
As: owner · Needs: DRV-056

1. On the history of the `.txt` file, pick the same change for **From** and **To** and compare.
2. Upload the `.png` twice (two different images, same name). Compare its two versions.

Expect:
- The page lets both through (see defect 21).
- Record what Lake answers: an empty diff or an error page. Confirm.

#### DRV-061 · Compare and resolve a drive conflict · P1
As: owner · Needs: DRV-CONFLICT

1. Open `TD-REPO`'s **Files** tab → **⋯** → **Conflicts**. Press **Compare preserved values**.
2. Type a merged text in **Prepared replacement text** and press **Use prepared text**.

Expect:
- The address is `/drive/conflict?repository_id=…&ref=main&path=…`.
- The heading reads "Two versions of <path>". The sub reads "Both versions are preserved. Compare
  them before choosing how to reconcile the file." The kind chip is on the right.
- An info banner is titled "Text overlap", with the explanation.
- Side cards: **Affected region**, **Version 1**, **Version 2** (Digest, Size) and **Preserved values**.
- **What differs** shows the text difference.
- Step 2: a toast reads "Conflict resolved". The browser opens the conflicted file's folder. The
  file holds the prepared text.

#### DRV-062 · A visitor cannot open a repository's Drive · P1
As: visitor · Needs: TD-REPO-PUBLIC

1. Open `/drive?repository_id=<TD-REPO-PUBLIC id>`, then `/drive/preview?…`, `/drive/history?…`
   and `/drive/share?…` for it.

Expect:
- Each goes to `/sign-in?next=<encoded address>`.

## Sharing a folder

#### DRV-063 · An organization repository to share from · P1
As: owner · Needs: TD-ORGWS, DRV-SUB

1. **New repository** in workspace `TD-ORGWS`, named `qa-<MMDD>-orgrepo`.
2. Open its **Files** tab and drop `DRV-SUB`'s `shared/` folder onto the drop zone. If the folder
   is not taken whole (see defect 11), make `shared` and `inner` with **New folder** and upload the
   files into each.

Expect:
- `shared/` holds `hello.txt`, `notes.md`, `empty.txt`, `blob.bin` and `inner/deep.csv`.

#### DRV-064 · The sharing page opens on the only workspace · P1
As: owner · Needs: DRV-063

1. Open `shared`. Press **Share**.

Expect:
- The account of `TD-ORG` has one workspace, so the address is replaced at once with
  `…&workspace_id=<TD-ORGWS id>`.
- The heading reads "Share shared". The sub reads "Review who can access this folder, then deliver
  it as a live folder, a continuing link, or an independent copy."
- The header has **Share with person** and **Create link** (primary).
- Figures:
  - **With access**: "<d> direct, <i> inherited".
  - **Active links**: 0, "0 in workspace <name>".
  - **Expired or revoked**: 0, "no longer open the folder".
- **Who Can Access This Folder Now** has the columns **Who**, **What they can do** (role chip +
  actions) and **How access arrives** (`direct` or `inherited`).
- **Choose a Delivery** has four cards:
  - **People**, labelled "live", with **Share with person**.
  - **Organization**, "live", with **Share with organization**.
  - **Link**, "live", with **Create link**.
  - **Independent Copy**, "not live", reading "Downloaded bytes stop being a live object. They
    cannot be updated or recalled by changing access here." It has no button.
- **Folder Share Management** reads "Links held in workspace **<name>**". The empty table reads
  "No links" / "A link lets anyone holding it open this folder." with **Create link**.

#### DRV-065 · Choose the workspace that holds the links · P1
As: owner · Needs: TD-REPO, TD-W2, TD-ORGWS

1. In the top bar, switch the workspace to `TD-ORGWS`.
2. Open `TD-REPO`'s **Files** tab and press **Share**.
3. Press the `W2` card.

Expect:
- Step 2: the address has no `workspace_id`. The heading reads "Share /". The sub reads "A folder's
  links are held in one workspace. Choose the workspace whose links you want to see or create."
- One card per live workspace of `owner`'s account (`W1`, `W2`), labelled "Workspace", reading
  "Hold this folder's links here".
- Step 3 opens the sharing page with `workspace_id=<W2 id>`. **Folder Share Management** reads
  "Links held in workspace **W2**. Other workspaces: **W1**". **W1** is a link.

#### DRV-066 · Share a folder with a person · P0
As: owner · Needs: DRV-064

1. Press **Share with person**.
2. Look at the button with the field empty. Click the field and pick `member` from the suggestions.
3. Open **What they can do**. Keep "View — browse and download". Press **Share with person**.

Expect:
- The modal is titled "Share with a Person". Its subtitle is the People card's sentence.
- **Person (email or handle)** is focused, with the placeholder "ana@example.com or @ana".
- The help reads "Choose one of the <n> people of this space, or type an email or handle." While
  the list loads, it reads "Loading the people of this space…".
- The roles are "View — browse and download", "Edit — upload, rename, move, and delete" and "Share —
  browse and create shares".
- A quiet banner reads "Lake records the share as a signed statement and keeps its digest on the
  grant as evidence."
- Step 2: the button is disabled while the field is blank.
- Step 3: a toast reads "Folder shared". The modal closes. The access table gains `member`'s
  identity with `read` and `direct`.

#### DRV-067 · Share with someone Lake cannot place · P2
As: owner · Needs: DRV-064

1. Press **Share with person**. Type `nobody-qa-<MMDD>@example.com`. Press **Share with person**.

Expect:
- Lake's message shows in red inside the modal (`role="alert"`). Nothing is granted.

#### DRV-068 · Share with an organization · P2
As: owner · Needs: DRV-064, TD-ORG

1. On the **Organization** card, press **Share with organization**.
2. Type `@<TD-ORG handle>`, choose "Edit — upload, rename, move, and delete", and share.

Expect:
- The modal is titled "Share with an Organization". **Organization (handle)** has the placeholder
  `@studio`. The role field is labelled "What it can do".
- A toast reads "Folder shared". The table shows only the identities that actually have access
  (confirm).

#### DRV-069 · Create a link · P0
As: owner · Needs: DRV-064

1. Press **Create link**.
2. Type `2020-01-01T00:00:00Z` in **Until when (UTC)** and press **Create link**.
3. Type a time a year ahead, such as `<next year>-12-31T23:59:59Z`, and press **Create link**.
4. Press **Copy** on the notice. Paste it somewhere private for the visitor cases.

Expect:
- The modal is titled "Create a Link", subtitle "Anyone holding the link can open this live folder
  until its expiry or revocation."
- **What link holders can do** offers "View — browse and download" and "Write scope —
  capability-aware clients only".
- **Passcode** is disabled, with the placeholder "Not available for drive links" and the help
  "Passcodes are not supported for drive-share links. No passcode is sent or stored."
- Step 2: Lake's refusal for a past expiry shows in red in the modal (`invalid_input` at
  `/expires_at`, confirm the text).
- Step 3: the modal closes. A notice "Copy this link now" reads "It is shown once; Lake keeps only
  its hash." It has a copy block with the full address `https://<host>/share/<token>…`.
- **Active links** reads 1. The table row reads "Anyone with this link", "Browse and download", the
  expiry, and `active`. It has **Details** and **Revoke link**.
- A reload removes the notice. The token is not shown again.

#### DRV-070 · A short-lived link and a write-scope link · P2
As: owner · Needs: DRV-064

1. Create a link that expires three minutes from now. Copy it.
2. Create a link with "Write scope — capability-aware clients only". Copy it.

Expect:
- The write link's row reads "Read; write scope is available to capability-aware clients".
- Keep both for DRV-078 and DRV-079.

#### DRV-071 · A link's own page · P1
As: owner · Needs: DRV-069

1. On the active link's row, press **Details**.

Expect:
- The address is `/drive/share/link?…&workspace_id=…&link_id=…`.
- The heading reads "Link for shared". The sub reads "Who can use the link, what it allows, when it
  stops, and what revocation changes."
- **Back to folder shares** is in the header.
- The card **Link** (title `active`) lists:
  - Who can access: "Anyone holding this bearer link".
  - What they can do, Until when, State, and Workspace (an id).
- The side card **Revoke this link** reads "Revoking prevents this link from opening the live folder
  on its next use. It does not recall files someone already downloaded." with **Revoke link**.
- Changing `link_id` to a made-up id shows the error page with eyebrow "not_found" and the message
  "drive share link was not found".

#### DRV-072 · Someone who cannot share sees a refusal · P1
As: member · Needs: TD-REPO

1. Open `TD-REPO`'s **Files** tab and press **Share**.

Expect:
- An error page with eyebrow "unauthorized", heading "We Could Not Load This Page", sub "principal
  is not allowed to share drive folder" and **Back to repositories**.
- With `&workspace_id=<W1 id>` added to the address, the page also fails with Lake's refusal.

## Public share pages

#### DRV-073 · A visitor opens a shared folder · P0
As: visitor · Needs: DRV-069

1. In a private window, open the link from DRV-069.

Expect:
- The page has no rail and no top bar, and asks for no sign-in.
- Eyebrow "Shared drive folder", heading "shared", sub `shared`.
- A quiet banner reads "View – browse and download. Access ends when this link expires or is
  revoked. Downloaded copies are independent."
- The toolbar has **Filter by name…** and "<n> of <n> items".
- The table (caption "Shared folder contents") lists `inner` with **Open folder**, and each file with
  **Download**.
- No control links to a repository page or to sign-in.

#### DRV-074 · Move down and back up in a shared folder · P1
As: visitor · Needs: DRV-073

1. Press **Open folder** on `inner`.
2. Press **Up one folder**. Open `inner` again by its name, then press **Shared folder**.

Expect:
- Step 1: the heading reads "inner". The header has **Shared folder** and **Up one folder**. The
  address keeps the token and `ref=main` and adds `path=shared/inner`.
- Step 2: both return to `shared`. At the shared root neither button shows.

#### DRV-075 · Preview and download a shared file · P1
As: visitor · Needs: DRV-073

1. Press `notes.md`. Press **Back to folder**.
2. Press `blob.bin`.
3. Press **Download** on `hello.txt`, `empty.txt` and `blob.bin`. Compare each with the original.

Expect:
- Step 1 opens `/share/<token>/preview?ref=main&path=shared/notes.md`. Eyebrow "Shared file
  preview", heading "notes.md". The sub has the format chip and the size. The Markdown is rendered.
  The header has **Back to folder** and **Download**.
- Step 2: "No preview is available." with **Download**.
- Step 3: each download is byte for byte identical, the empty file included (0 bytes).

#### DRV-076 · Filter a shared folder · P2
As: visitor · Needs: DRV-073

1. Type `hel`, then `zzzz`.

Expect:
- `hel` leaves `hello.txt`. `zzzz` reads "Nothing matches" / "No item here is named like “zzzz”."
- The **Kind** column reads `tree` and `blob`, not folder and file (see defect 19).

#### DRV-077 · A link never reaches above its folder · P1
As: visitor · Needs: DRV-073

1. Change the address's `path=` to the repository top (`path=`), then to a sibling folder name,
   then to `shared/../`.
2. Change a preview address's `path` to a file outside `shared/`.

Expect:
- Every one shows the error page (confirm for the empty `path=`, which Lake may read as the
  repository top): eyebrow "Shared drive folder", heading "Shared folder", "This link
  does not open a folder" and Lake's message in red. No file action shows.

#### DRV-078 · An expired link · P2
As: visitor · Needs: DRV-070

1. Open the three-minute link. Wait until it has expired. Press any file's name, or reload.

Expect:
- Before the expiry the folder opens. After it, the error page shows.
- The owner's sharing page shows that link as `expired`, counted in **Expired or revoked**.

#### DRV-079 · A write-scope link is read-only in the browser · P2
As: visitor · Needs: DRV-070

1. Open the write-scope link.

Expect:
- The same read-only page: browse, preview and **Download** only. The banner still reads "View –
  browse and download. …".

## Shared with Me

#### DRV-080 · Folders shared with me · P1
As: member · Needs: DRV-066

1. Open **Data → Shared with Me**.
2. Press the folder's name.

Expect:
- Eyebrow "Data", heading "Shared with Me", sub "Live folders shared directly with your identity. A
  move keeps the same folder identity; a downloaded copy remains independent."
- Figures: **Shared folders** ("shared with your identity"), **Available** ("open now") and
  **Removed** ("no longer reachable").
- The row reads `qa-<MMDD>-orgrepo/shared`, "at main", **Shared by** (an id), **What you can do**
  (the actions) and the state `available` in green.
- Links from DRV-069 are not listed.
- Step 2 opens `/drive?repository_id=…&ref=main&path=shared` and lists its files.

#### DRV-081 · Search and filter Shared with Me · P2
As: member · Needs: DRV-080

1. Type `shared` in **Search folders…**, then `zzzz`.
2. Press **Removed**, then **All**.

Expect:
- Step 1 keeps the row, then reads "No folder matches" / "Clear the search or the filter to see more."
- The chips read **All**, **Available <n>** and **Removed <n>**.

#### DRV-082 · A shared folder that is gone · P2
As: owner, then member · Needs: DRV-080

1. As `owner`, move `shared` into a new folder `moved`. As `member`, reload Shared with Me.
2. As `owner`, delete `moved/shared`. As `member`, reload.

Expect:
- Step 1: the row now reads `qa-<MMDD>-orgrepo/moved/shared` and still opens. The share follows the
  folder, not its path.
- Step 2: the state reads `removed` in red. The name is plain text, not a link.

#### DRV-083 · Shared with Me for other viewers · P2
As: newcomer, then operator

1. As `newcomer`, open `/shared-with-me`.
2. As `operator`, open `/shared-with-me`.

Expect:
- Step 1: "Nothing shared with you yet" / "No folders are shared directly with you."
- Step 2: "Not Available" / "This page belongs to an account; the operator credential has no
  account." with **Home**. The rail has no **Shared with Me** item for the operator.

## Revoking links

#### DRV-084 · Revoke a link from its page · P1
As: owner · Needs: DRV-071

1. On the link's page, press **Revoke link**. Press **Escape**.
2. Press **Revoke link**, then **Revoke link** in the dialog.

Expect:
- The dialog is titled "Revoke this link?". It reads "The link stops opening the live folder on its
  next use. Files someone already downloaded stay with them."
- Step 1 does nothing.
- Step 2: a toast reads "Link revoked". The card's state reads `revoked`. The side shows "This link
  no longer opens the folder." instead of the revoke card.

#### DRV-085 · A revoked link stops at the next click · P0
As: visitor, with owner in another browser · Needs: DRV-069

1. As `visitor`, open a fresh active link (create one as `owner` if DRV-084 used it). Leave the
   folder page open.
2. As `owner`, revoke that link.
3. As `visitor`, without reloading, press **Download** on a file, then a folder's **Open folder**.

Expect:
- Both are refused. **Download** fails, and the folder shows the error page "This link does not
  open a folder".
- A file downloaded before the revoke is still on the visitor's disk; nothing recalls it.

#### DRV-086 · Revoke from the links table · P1
As: owner · Needs: DRV-070

1. On the sharing page, press **Revoke link** on an active row.
2. Look at the rows that are already `expired` or `revoked`.

Expect:
- Step 1 revokes at once, with no confirmation dialog (see defect 1). A toast reads "Link revoked".
  The row reads `revoked`.
- Step 2: those rows still offer **Revoke link** (see defect 1). Press it on one and record what Lake answers.
- A refusal shows as a red banner "Revoke refused" with the message.

#### DRV-087 · A link to a repository's top folder · P2
As: owner, then visitor · Needs: DRV-063

1. As `owner`, open `qa-<MMDD>-orgrepo`'s **Files** tab at its top. Press **Share** and create a link.
2. As `visitor`, open it and press **Open folder** on `shared`.

Expect:
- Step 1: the heading reads "Share /".
- Step 2: the `shared` folder opens. **Shared folder** and **Up one folder** are missing, though
  this is below the shared root (see defect 2).

## Deleting drives

#### DRV-088 · Delete a drive: wrong name, then right · P0
As: owner · Needs: DRV-002

1. In `qa-<MMDD>-files`, open **⋯** → **Drive settings**.
2. In "Type “qa-<MMDD>-files” to confirm", type `qa-<MMDD>-FILES`.
3. Type `qa-<MMDD>-files` and press **Delete drive**.

Expect:
- **Delete This Drive** reads "Every folder and file in it is deleted and cannot be restored."
- Step 2: **Delete drive** stays disabled.
- Step 3: a toast reads "Drive “qa-<MMDD>-files” deleted". The browser is on `/w/<W1>/drives`. The
  card is gone, and **Recent Files** no longer lists its files.
- There is no dry-run list of what goes (see defect 22).

#### DRV-089 · Deleting a repository drive keeps the repository · P2
As: owner · Needs: DRV-005

1. In `qa-<MMDD>-repolink`, open **Drive settings**. Type its name and press **Delete drive**.

Expect:
- The text reads "The drive goes; its repository stays, with every file and its history. Delete the
  repository from its own page."
- After the delete, `TD-REPO` is unchanged: its home page lists the same files and changes.

#### DRV-090 · Opening a drive without an id · P2
As: owner

1. Open `/w/<W1>/drives/drive`.

Expect:
- The heading reads "Drive". "No drive chosen" / "Open a drive from the Drives page." with
  **All drives**.

## Phone width and keyboard

#### DRV-091 · Drives at phone width · P2
As: owner · Needs: DRV-033

1. At 390×844, open **Data → Drives**, both tabs, and `qa-<MMDD>-repodrive`.
2. Press **Upload**, then open a row's **⋯** menu near the bottom of the screen.

Expect:
- The drive cards are one per row. The Storage table scrolls inside its box.
- The file manager's table scrolls sideways inside its region. The selection bar wraps.
- The menu opens fully on screen, at least 8 px from the right edge. Record whether a menu on the
  last row runs off the bottom.
- Dragging rows does nothing on touch. **Move** in the menu works.

#### DRV-092 · Sharing and public pages at phone width · P2
As: owner, then visitor · Needs: DRV-069

1. At 390×844, open the sharing page, then the public link.

Expect:
- The delivery cards stack one per row. The access and links tables scroll inside their boxes.
- The **Create a Link** modal's buttons stay reachable with the on-screen keyboard open.
- The public folder's **Download** and **Open folder** buttons are reachable without sideways scroll.

#### DRV-093 · The file manager by keyboard · P2
As: owner · Needs: DRV-033

1. In `qa-<MMDD>-repodrive`, Tab to a row's checkbox and press **Space**.
2. Tab to its **⋯** menu and press **Enter**. Press **Escape**.
3. Tab to a folder's name and press **Enter**.

Expect:
- Step 1 selects the row, and the selection bar appears. Rows themselves never take focus; selection
  by keyboard is only through the checkboxes.
- Step 2: the menu opens with focus on its first item. Escape closes it.
- Step 3 opens the folder.

## Suspected defects

| # | Where | What looks wrong | Case that shows it |
|---|---|---|---|
| 1 | `frontend/src/pages/repositories/DriveSharePage.tsx:275` | **Revoke link** on every row, including expired and revoked links, revokes at once with no confirmation. The guide (`docs/guide/sharing.md:113`) says to confirm. | DRV-086 |
| 2 | `frontend/src/pages/repositories/SharePage.tsx:34` | `nested` needs `root_path !== ""`. Below a shared repository top, there is no **Up one folder** or **Shared folder**. | DRV-087 |
| 7 | `frontend/src/pages/drives/DrivePage.tsx:1008-1016` | **Remove** on Unfinished Uploads has no `catch`: a failure says nothing and stops at the first upload. | DRV-026 |
| 8 | `frontend/src/pages/drives/StorageTab.tsx:46, 190` | One busy flag for every row: **Show connection** spins on every Ready row at once. | DRV-038 |
| 9 | `frontend/src/pages/drives/ShortcutDialog.tsx:35` | "Item in another drive" offers storage drives, while its help says workspace-files or virtual drives. A storage item's key is a path, not an entry id. | DRV-030 |
| 10 | `frontend/src/pages/drives/DrivesPage.tsx:95` | With only a kind chip and no text, the empty state reads "No drive is named like “” among …". | DRV-008 |
| 11 | `frontend/src/pages/repositories/DrivePage.tsx:316-318, 353` | The repository upload encodes the whole selection in memory before checking 64 MiB. Its drop zone does not walk a dropped folder. | DRV-048, DRV-063 |
| 12 | `frontend/src/pages/repositories/DrivePage.tsx` | No write check: Upload, New folder, Rename, Move, Delete and Share show to readers, who see only Lake's refusal. | DRV-052 |
| 17 | `frontend/src/pages/repositories/DriveHistoryPage.tsx:33`, `DriveDiffPage.tsx:21`, `DrivePreviewPage.tsx:21` | Tab titles differ from the registry: "File History" (registry "History"), "Version Comparison" ("Compare Versions"), "Files" ("Preview"). | DRV-053, DRV-056, DRV-057 |
| 19 | `frontend/src/pages/repositories/SharePage.tsx:92` | The public Kind column shows `tree` and `blob`; the Drive pages say `folder` and `file`. | DRV-076 |
| 21 | `frontend/src/pages/repositories/DriveHistoryPage.tsx:70-80, 137-148` | Compare allows **From** equal to **To**, and binary versions. | DRV-060 |
| 22 | `frontend/src/pages/drives/DrivePage.tsx:858-991`, `StorageTab.tsx:240-296`, `pages/repositories/DrivePage.tsx:486-507` | Drive, item, storage and repository-drive deletes do not use the shared `DeletionDialog` (`docs/ui/deletion.md`): no dry-run list of what goes. Item deletes have no typed text. | DRV-025, DRV-088 |
| 23 | `docs/guide/drives.md:93` | The guide says `upload_drive_entries` takes 16 MiB a call; the code sends 12 MiB batches of files up to 4 MiB. | DRV-013 |
| 24 | `docs/guide/drives.md:176` | The guide says Lake offers volumes only. New storage offers your own buckets and the `zetta` and `lake` providers. | DRV-040 |
| 26 | `frontend/src/pages/drives/NewDrive.tsx:133-141` | A new repository drive's **Repository name** has no format check, unlike New Repository. | DRV-004 |
| 28 | `frontend/src/routes.tsx:199` | `isServerRoute` lacks `/drive/files/download`, so a Markdown link to a workspace file is routed in the app. | DRV-016 |
| 29 | `frontend/src/pages/repositories/DriveSharePage.tsx:189`, `SharedWithMePage.tsx:89` | **Who** and **Shared by** show raw TAAP ids, though names are available. | DRV-066, DRV-080 |
| 30 | `frontend/src/pages/drives/stores.ts:54` | The **Storage** figure counts stores being deleted; the other figures do not. | DRV-044 |
| 31 | `frontend/src/pages/drives/StorageTab.tsx:326` | New storage's repository list includes repositories with no workspace. | DRV-039 |
| 33 | `frontend/src/pages/repositories/DrivePage.tsx` vs `frontend/src/pages/drives/DrivePage.tsx` | Two file managers act differently: one keeps sort and view in the address and has a grid; the other keeps sort local and has selection, drag and folder upload. | DRV-022, DRV-049 |
