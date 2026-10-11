# Search

Lake searches everything you can open: your repositories, the code in them, the datasets,
transcripts, projects, agents, models and drives of every workspace you have, and these Docs.
There are two ways in: the **Search** box in the top bar, for a quick look as you type, and
**Data → Search**, for every result with the places you choose.

## Search From the Top Bar

1. Click **Search** in the top bar, or press **⌘K** (Ctrl K) or `/` anywhere outside a text box.
   On a phone, tap the magnifier; the search fills the screen.
2. Type. **Go to** lists the pages and workspaces whose names match, at once. Under it, each place
   shows its best few results with your words marked and a muted line saying where each lives:
   the drive and folder of a file, the workspace when you have several, and for a transcript the
   words around the match.
3. Places fill in as they answer. One still searching says *Searching…*; one that failed says so
   in its own group, and the others are unaffected. A place with nothing to show is left out.
4. Use ↑ and ↓ to move across groups and Enter to open the result, or click it.
5. **See all results** (or ⌘ Enter) opens **Data → Search** with the same words and every place on.
   With no page matching, Enter does the same.

Try it with words you said in a meeting: typing *churn is up* finds every transcript, in any of
your workspaces, whose text says it.

## What It Searches

| Place | What matches | Where |
|---|---|---|
| **Repositories** | the name, description and owner of every repository you can open | everywhere |
| **Code** | the files in your repositories, ranked, as a substring or as a regular expression | everywhere |
| **Datasets** | dataset and database names | every workspace |
| **Transcripts** | the title and the whole text of each transcript | every workspace |
| **Projects** | project names, descriptions and decisions | every workspace |
| **Agents** | your own agents by name, id and purpose | every workspace |
| **Models** | trained models by name, id and methodology | every workspace |
| **Drives** | the names of drives and of the folders, files and shortcuts in them | every workspace |
| **Docs** | the title and text of every Docs page | everywhere |

The workspace places search every workspace you have, not only the one selected in the top bar.
When you have more than one, each result names its workspace. A workspace that could not be
searched (stopped, or starting) is named under the results of the others.

A place that fails or is unavailable says so in its own group and never hides the others.

## Search Everything on One Page

1. Open **Data → Search** (or **See all results** in the top bar's search).
2. Type in the box (*Search everything you chose below…*) and press Enter.
3. Under **Search in**, tick the places to search. **Everything** ticks them all; **Code only**
   leaves just code search.
4. Each result opens its page: a repository, a file in a repository, a dataset, a transcript, a
   project, an agent, the Models page, a drive's folder or file, or a Docs page.

The address keeps the query and the places (`/search?q=renewal&in=code,transcripts`), so a search
can be bookmarked or shared. This browser also remembers which places you ticked.

## Drives

The **Drives** place finds drives, folders, files and shortcuts by name, in every drive of the
workspace. Each result shows an icon for what it is, its name with your words marked, and under it
the drive and the folder that holds it (for example *Team / Trip / Day 1*).

- A drive or a folder opens in **Data → Drives**, at that folder.
- A file opens in the browser when the browser can show it (images, PDF, text, audio and video)
  and downloads otherwise; a repository drive's file opens in the repository's preview.
- A shortcut opens what it points at. A broken shortcut opens the folder that holds it, with the
  shortcut selected.

Names match anywhere in them, ignoring capitals: `report` finds *Q3 REPORT.pdf*. File contents
are not searched. A repository drive is searched by the names of its folders and files on `main`;
to search what is written inside them, use the **Code** place. A drive is searched only as far as
you can open it: a repository drive whose repository you cannot read is left out.

Inside one drive, the box on the drive's own page (*Search this drive…*) does the same for that
drive alone ([Drives](/docs/drives#find-a-file)).

## Code Search

Code search reads the search index Lake keeps over your repositories. It understands three kinds
of query:

| You type | It finds |
|---|---|
| `connection refused` | ranked full-text matches (BM25F over the content and the path) |
| `"connection refused"` | the exact text, as a substring |
| `/conn(ect)?ion/` | matches of a regular expression (ECMAScript syntax) |

Ranked results are ordered by score; quoted and regular-expression results all score `1` and are
ordered by repository and path.

### Code Options

Open **Code options** under the box to narrow code search, then press **Apply**:

- **Repository**: one repository, by its id.
- **Branch or tag**: search that branch's tip, or a release or tag snapshot. A tag's immutable
  snapshot wins over a branch of the same name.
- **Exact change**: search the files as they were at one change digest.
- **Scan complete history**: search every change in the repository's history.

An exact change and complete history are scans: they read old versions one by one instead of the
index, so they are slower and are limited to one repository.

### Search Inside a Published Artifact

Add `in:artifact:<name>@<version>` to the query to search the members of one build output: the
name is the output path and the version is its result digest. For example
`parser in:artifact:dist/app.tar@<result digest>`. Hits name the artifact, its version, the member
digest and the matching line. Binary members are listed by digest only, and only one level of
nested containers is indexed. An artifact search cannot be combined with a branch, an exact change
or complete history. The **Search inside** form on a repository's artifact page fills this in for
you.

### Code Search From the API

`search` is a read, so REST calls it with `GET` and its inputs as query parameters:

`GET /v1/search?query=connection&limit=20`

Narrow it to one repository's complete history:

`GET /v1/search?query=timeout&customer_id=<customer id>&repository=<repository id>&history=true`

The inputs:

| Input | Meaning |
|---|---|
| `query` | Required. Ranked text, `"quoted"` text or `/regex/`, with an optional `in:artifact:` scope |
| `customer_id` | The account whose repositories to search, as everywhere else (`customer` still works). Empty: every account you can read |
| `workspace_id` | Search only the index of one workspace |
| `repository` | One repository id |
| `ref` | A branch or tag name |
| `change` | An exact change digest; needs `customer` and `repository` |
| `history` | `true` walks every change of one repository; needs `customer` and `repository` |
| `limit` | Results to return, 20 by default, at most 1000 |

The answer's `mode` says which corpus answered:

| Mode | Corpus |
|---|---|
| `hot` | the current branch tips (the default) |
| `snapshot` | the immutable snapshot of a release or tag |
| `scan` | an exact change or the complete history; `scanned_changes` counts what was read |
| `artifact` | the members of one published output |

Each item names the account, repository, path, change, snippet, score and line, and the workspace
whose index answered (`workspace_id`).

From an AI agent the MCP tool is `search`, with the same inputs as JSON.

## Search the Other Places From the API

Each place is the capability its own page reads:

- **Repositories**: `list_repositories`, filtered by name.
- **Transcripts**: `list_transcripts` with `q`, which matches the title or the text, ignoring
  case. Each answer's `excerpt` is then the text around the match. Ask each workspace in turn to
  search them all, as the top bar does:
  `GET /v1/list_transcripts?customer_id=<customer id>&workspace_id=<workspace id>&q=renewal`
- **Datasets**: `workspace_sql` over the catalogue table:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "statement": "SELECT database, dataset FROM system.datasets"}
```

- **Projects, agents and models**: `workspace_query` with the engine read `list_projects`,
  `list_agents` or `list_models`:

`POST /v1/workspace_query`

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "list_projects", "input": {}}
```

- **Drives**: `search_drives` with `q`, and `drive_id` to search one drive:
  `GET /v1/search_drives?customer_id=<customer id>&workspace_id=<workspace id>&q=report`
- **Docs**: `list_docs` and `get_doc`, which need no sign-in.

## Who Can See What

- Search shows only what you could open yourself. Repository and code hits pass through the same
  policy check as opening the repository: a private repository you cannot read never appears.
- Signed-out visitors cannot use Search; they can browse public repositories from
  **Data → Repositories**.
- Workspace places read every workspace you have, each with `read` on the account that owns it.
- A search key or API token needs the `search` capability (or the read capability of the place) in
  its scope.

## Limits

- Code search returns at most 1000 results per call; the page asks for 100.
- Transcripts return 50 per page.
- Drives return the first 100 names on the page (`search_drives` answers 50 by default, at most
  200, and says when more matched).
- Repositories are matched among the first 500 the list returns.
- Datasets, projects, agents and models are matched by name and description, not by their
  contents. To look inside a dataset, query it ([Datasets and Loading Data](/docs/load-data#query-a-dataset)).

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| A workspace in `building_workspaces` | Its index is being built (after a move or because it was lost). It answers nothing yet; search again later. |
| A workspace in `stale_workspaces` | A write to its index failed. It answers what it held before, and Lake retries the write on its own. |
| A workspace in `skipped_workspaces` | The workspace is stopped, or its store is moving or unavailable, so its repositories were left out. Start it on **Ship → Workspaces**. |
| `invalid_input` at `/workspace_id` | The workspace is not one whose index can be searched, or is not in an account you can read. |
| `invalid_input` naming `change` or `history` | An exact change and complete history need `customer` and `repository`, and cannot be combined with `ref` or with each other. |
| *Searching … is not available on this Lake yet* | That place's capability is not on this Lake. The other places still answer. |
| `unauthenticated` (401) | Sign in again, or send a valid key. |

## Related Pages

- [Repositories](/docs/repositories)
- [Reviews, Issues and Releases](/docs/repository-collaboration)
- [Datasets and Loading Data](/docs/load-data)
- [Transcripts and Speech-to-Text](/docs/transcripts)
- [Models and Search Indexes](/docs/ship): searching a dataset through a published search index
- [REST API and OpenAPI](/docs/developer)
