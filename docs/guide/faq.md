# FAQ and Troubleshooting

Answers to the questions people ask most, and what to do about the messages lake shows. Each answer
links the page with the details. An error from the API always carries a `code` and, when your input
is at fault, a `path` to the field; [REST API and OpenAPI](/docs/developer#error-codes) lists every
code.

## Workspaces

### Why does lake say `workspace_id is required`?

A workspace is where your data, projects, runs and models live, and the account that owns your
workspaces can have several. lake never guesses which one you mean, so every Design, Train, Ship,
Transcripts and Ask Orama call names one.

- In the web app, the workspace is in the address: `/w/<workspace id>/…`. The picker in the top bar
  changes it.
- From a script or an agent, find yours with `GET /v1/list_workspaces?customer_id=<customer id>`
  (the MCP tool `list_workspaces`), or call under the prefix `/w/<workspace id>/v1/…`, which fills it
  in.

If an MCP client cached its tool list before an upgrade, reconnect it so it sees the field.
Details: [Workspaces](/docs/workspaces), [The Workspace Prefix](/docs/developer#the-workspace-prefix).

### What does `workspace_not_ready` mean?

The workspace exists but its Zetta is not ready: it is being prepared, it was stopped, or it was
never started. A Design, Train or Ship page shows which:

- **Preparing Your Workspace**: wait; the page checks again every 15 seconds.
- **Start Your Workspace**: press **Start your workspace**.
- **Your Workspace Could Not Be Prepared**: the operator's message says why; contact whoever runs
  your lake.

From the API, poll `GET /v1/workspace_status?customer_id=<customer id>&workspace_id=<workspace id>`
until `ready` is true. Details: [Workspaces](/docs/workspaces).

### I have no workspace yet. How do I start one?

Open **Ship → Workspaces** and press **New workspace**: give it a name and an environment (`dev`,
`staging`, `prod` or another lowercase name). A Design, Train or Ship page with no
workspace to open says *No Workspace Yet* and links to Workspaces. Through the API,
`POST /v1/start_workspace` creates a first workspace, named `workspace` in `prod`, when you have
none, and answers its `workspace_id`:

```json
{"customer_id": "<customer id>"}
```

Details: [Workspaces](/docs/workspaces).

### How do I delete a workspace, and what goes with it?

Open **Ship → Workspaces**, press **Delete** on its row, read the list of everything that goes with
it (its Zetta and every record in it, its repositories, apps, unfinished builds and images, with
counts), type the workspace's name and press **Delete workspace and everything in it**. It cannot
be undone, and it needs `admin` on the account that owns the workspace.

To see the list without deleting, `POST /v1/delete_workspace` with `dry_run`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "dry_run": true}
```

Details: [Workspaces](/docs/workspaces).

### Where is my data stored?

What you put in a workspace (datasets, runs, models, transcripts, Ask Orama conversations,
repository descriptions, issues and reviews, secrets, the workspace's own audit) is kept in that
workspace's own Orama Zetta, sealed with the workspace's key. lake's central database keeps only
accounts, memberships, grants, sign-in and the routing that says which workspace holds what.
Recordings sent for speech-to-text are never kept. Details: [Security Model](/docs/security#where-your-data-lives).

## Signing In and Keys

### Why does the page say *Your session has ended*?

Your session expired, was revoked (on **Sessions**, or by revoking its device), or you signed out
elsewhere. Press **Sign in again**; you come back to the same page. Details:
[Your Account and Sign-In](/docs/account).

### What do the `unauthenticated` reasons mean?

A `401 unauthenticated` answer carries a `reason`:

- `missing`: no `Authorization: Bearer` header and no session. Check that your script sends the
  header and that `LAKE_KEY` is set where it runs.
- `expired`: the key's lifetime passed, or the sign-in that made it ended. Make a new key.
- `invalid`: the key is mistyped, revoked or from a revoked device.

Details: [REST API and OpenAPI](/docs/developer#when-a-key-is-refused), [Tokens and Keys](/docs/tokens).

### What is the difference between `unauthorized` and `not_found`?

`unauthorized` (403) means lake knows who you are, but you, or the key you used, may not make this
call on this object: ask an administrator for a grant, or use a key that names the capability.
`not_found` (404) means the object does not exist, or you cannot see it; lake does not tell the two
apart, so it never reveals what exists to someone who may not see it. Details:
[Grants and Effective Access](/docs/access).

### Why does a repository link send me to sign in?

The repository is not public. Signed out, you see only public repositories, and any other address
sends you to sign in without saying whether it exists. Sign in with an account that has access, or
ask its owner to share it. Details: [Repositories](/docs/repositories).

### My script works with the browser's cookie and fails from another site. Why?

A `POST` that only lake's session cookie authenticates must come from lake's own origin and be
`application/json`; otherwise it answers `unauthorized` with *Cross-site requests cannot use the
browser session.* This stops a page on another site from acting as you. Scripts and apps send a key
(`Authorization: Bearer <key>`), which works from anywhere. Details:
[Security Model](/docs/security#cross-site-requests).

## Data

### Why was my upload refused?

**Data → Datasets → Load data** takes one CSV or JSONL file of up to 16 MiB. It refuses:

- **A name SQL cannot use.** A dataset name is letters, digits and underscores, starting with a
  letter or an underscore, at most 63 characters, and not a SQL keyword. `accounts-2026` is refused;
  `accounts_2026` is fine.
- **Malformed CSV**, naming the line: a header that leaves a column unnamed or names one twice, a
  quoted field that does not close on its line, or a row with more or fewer fields than the header
  (`line 3: 3 fields, but the header has 2 columns`).
- **A file over 16 MiB.** Split it, or load it from a database connection instead.

Details: [Datasets and Loading Data](/docs/load-data).

### The name is taken. What now?

A dataset name that already exists answers `conflict` and nothing is loaded. Tick *If the name is
taken, save it under the next free name* to have it saved as `accounts_2`; the page then says it was
renamed. Through the API, add `"if_exists": "rename"` to the upload's `input`, for example
`POST /v1/workspace_act`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "upload_dataset", "input": {"table_name": "accounts", "format": "csv", "content": "id,plan\n1,pro\n", "if_exists": "rename"}}
```

The answer's `renamed_from` says what you asked for. A project name that is taken is refused the same
way: choose another name. Details: [Datasets and Loading Data](/docs/load-data).

### Why are Record and Upload audio turned off?

This lake has no speech-to-text configured: the page says *Speech-to-text is not configured on this
lake.*, and the API answers `not_supported`. Uploading a text, `.vtt` or `.srt` file and pasting
still work. Ask whoever runs your lake to configure speech-to-text. Details:
[Transcripts and Speech-to-Text](/docs/transcripts).

### A long recording failed with a 504. What happened?

lake waits while the audio is transcribed, but the proxies in front of lake close a response after
about 60 to 100 seconds. A recording that takes longer to transcribe answers 504 even though lake
finishes the call. Short recordings fit well within it; split a long one, or upload a transcript the
recorder already made (`.vtt`, `.srt` or text). Audio is limited to 25 MiB. Details:
[Transcripts and Speech-to-Text](/docs/transcripts).

### Why is my storage full (`quota_exceeded`)?

The account that owns your workspaces has a storage limit for repository content. New writes are
refused once it is full; everything stored stays readable. **Organization → Usage** shows what is
stored and the limit; an administrator of the account can change it there with **Set storage
limit**. Details: [Usage and Metering](/docs/usage).

## Design and Train

### Why won't my pipeline run (`not_approved`, `gate_blocked`)?

- `not_approved`: a pipeline must be approved before it runs, and again after every change. Open the
  pipeline and press **Approve**.
- `gate_blocked`: a check refused the run, and nothing ran. The message names the check; **Check**
  on the pipeline page lists every check with its reason.

Details: [Projects and Pipelines](/docs/design), [Train a Model](/docs/train).

### Why does my run have no held-out score, or say *do not quote*?

A run measures its model on rows it kept out of training (**Hold out for measuring**, 0.2 by
default). With a hold-out of 0 nothing is measured, and every score is *in-sample*, which flatters.
A held-out score marked **do not quote** carries a caveat from the engine, such as a score measured
on too few rows. Run again with a hold-out and more data. A pipeline whose methodology does not run
yet says *Trains nothing yet*. Details: [Train a Model](/docs/train).

### Why is an agent unavailable?

The Agent Library (**Design → Agent Library**) says, for each agent Orama provides, whether it is
**Available** here. An agent that is not deployed on your lake is shown as such, and the pages that
use it (for example **Run an Experiment** on Train → Experiments) explain that it is unavailable and
disable sending. Details: [Agents](/docs/agents).

### Why was my confirmation refused?

lake runs only the exact proposal you reviewed:

- Ask Orama confirms only a proposal it stored in your own conversation; anything else answers
  *That proposal is not in this conversation.*
- ApplicationAgent and ExperimentsAgent read the task again on **Confirm**: if the proposal changed,
  is no longer a finished proposal, or (for a study) the edited JSON does not parse, nothing starts.
  A public app also needs its own *I understand this app will be public* box ticked.

Ask again for a fresh proposal. Details: [Ask Orama](/docs/ask-orama), [Apps](/docs/apps),
[Experiments](/docs/experiments).

## Limits and Errors

### What do `rate_limited` and `busy` mean?

Both mean retry. `rate_limited` (429) means too many of your long requests (live updates, event
waits) are open at once, 64 per caller; close tabs that follow live updates, or wait for
`Retry-After`. `busy` (503) means the server or a workspace is momentarily full. Details:
[REST API and OpenAPI](/docs/developer#limits).

### My request is too large (`size_invalid`).

A request body is at most 64 MiB of JSON, and some values have their own limits: a dataset upload
16 MiB, audio 25 MiB, a transcript's text 400,000 bytes. Base64 inside JSON is a third larger than
the file. Details: [REST API and OpenAPI](/docs/developer#limits).

### A workspace call failed with `engine_error`.

The workspace's engine could not complete the call, and the message is its own. Engine reads time
out after 15 seconds and actions after 60. Check the message; if the workspace was just started, wait
until it is ready. Details: [Workspaces](/docs/workspaces).

## Getting Help

### Where can an agent learn lake?

`/llms.txt` describes lake in one page, `/llms-full.txt` adds every tool with its input schema, and
the lake skill (`/skills/lake/SKILL.md`) teaches an agent the flows and rules. All three are public.
Details: [MCP Setup](/docs/mcp#teach-your-agent).

### Where else can I look?

- **Search** in the Data section searches these Docs alongside your repositories, datasets and
  transcripts.
- **Ask Orama**, at the top of every page, answers questions from what you can read in lake.
- **Organization → Activity** shows what you did and what was refused, with the reason.
- `GET /v1/server_info` names the lake version and revision to quote when you report a problem.

## Related Pages

- [Getting Started](/docs)
- [Workspaces](/docs/workspaces)
- [REST API and OpenAPI](/docs/developer)
- [MCP Setup](/docs/mcp)
- [Security Model](/docs/security)
