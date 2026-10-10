# Search

**Data → Search** looks for one query across the places you choose: your repositories, the code
in them, the datasets and transcripts of your workspace, its projects, agents and models, and these
Docs. Every place is searched at once and the results come back grouped by place, each group with
its count.

## What It Searches

| Place | What matches | Needs a workspace |
|---|---|---|
| **Repositories** | the name, description and owner of every repository you can open | no |
| **Code** | the files in your repositories, ranked, as a substring or as a regular expression | no |
| **Datasets** | dataset and database names in the workspace | yes |
| **Transcripts** | the title and text of the workspace's transcripts | yes |
| **Projects** | project names, descriptions and decisions | yes |
| **Agents** | the workspace's own agents by name, id and purpose | yes |
| **Models** | trained models by name, id and methodology | yes |
| **Docs** | the title and text of every Docs page | no |

The places that need a workspace search the workspace selected in the top bar. With none selected,
the page says *Choose a workspace in the top bar to search its datasets, transcripts, projects,
agents and models*, and still searches the rest.

A place that fails or is unavailable says so in its own group and never hides the others.

## Search Everything

1. Open **Data → Search**.
2. Type in the box (*Search everything you chose below…*) and press Enter.
3. Under **Search in**, tick the places to search. **Everything** ticks them all; **Code only**
   leaves just code search.
4. Each result opens its page: a repository, a file in a repository, a dataset, a transcript, a
   project, an agent, the Models page or a Docs page.

The address keeps the query and the places (`/search?q=renewal&in=code,transcripts`), so a search
can be bookmarked or shared. This browser also remembers which places you ticked.

## Code Search

Code search reads the search index lake keeps over your repositories. It understands three kinds
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

`GET /v1/search?query=timeout&customer=<customer id>&repository=<repository id>&history=true`

The inputs:

| Input | Meaning |
|---|---|
| `query` | Required. Ranked text, `"quoted"` text or `/regex/`, with an optional `in:artifact:` scope |
| `customer` | The account whose repositories to search (its id, the same value as `customer_id` elsewhere). Empty: every account you can read |
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
  case:
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

- **Docs**: `list_docs` and `get_doc`, which need no sign-in.

## Who Can See What

- Search shows only what you could open yourself. Repository and code hits pass through the same
  policy check as opening the repository: a private repository you cannot read never appears.
- Signed-out visitors cannot use Search; they can browse public repositories from
  **Data → Repositories**.
- Workspace places read the workspace selected in the top bar, with `read` on the account that owns
  it.
- A search key or API token needs the `search` capability (or the read capability of the place) in
  its scope.

## Limits

- Code search returns at most 1000 results per call; the page asks for 100.
- Transcripts return 50 per page.
- Repositories are matched among the first 500 the list returns.
- Datasets, projects, agents and models are matched by name and description, not by their
  contents. To look inside a dataset, query it ([Datasets and Loading Data](/docs/load-data#query-a-dataset)).

## Troubleshooting

| What you see | Why, and what to do |
|---|---|
| A workspace in `building_workspaces` | Its index is being built (after a move or because it was lost). It answers nothing yet; search again later. |
| A workspace in `stale_workspaces` | A write to its index failed. It answers what it held before, and lake retries the write on its own. |
| A workspace in `skipped_workspaces` | The workspace is stopped, or its store is moving or unavailable, so its repositories were left out. Start it on **Ship → Workspaces**. |
| `invalid_input` at `/workspace_id` | The workspace is not one whose index can be searched, or is not in an account you can read. |
| `invalid_input` naming `change` or `history` | An exact change and complete history need `customer` and `repository`, and cannot be combined with `ref` or with each other. |
| *Searching … is not available on this lake yet* | That place's capability is not on this lake. The other places still answer. |
| `unauthenticated` (401) | Sign in again, or send a valid key. |

## Related Pages

- [Repositories](/docs/repositories)
- [Reviews, Issues and Releases](/docs/repository-collaboration)
- [Datasets and Loading Data](/docs/load-data)
- [Transcripts and Speech-to-Text](/docs/transcripts)
- [Models and Search Indexes](/docs/ship): searching a dataset through a published search index
- [REST API and OpenAPI](/docs/developer)
