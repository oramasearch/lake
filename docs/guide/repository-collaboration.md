# Reviews, Issues and Releases

A repository is also where a team works together: changes are reviewed before they land, work is
tracked as issues, conflicting edits are resolved, build results are recorded with their evidence,
and releases tie a tag to verified artifacts. This page covers each of those.

The web app shows all of them on the repository's tabs. Issues are opened, discussed and changed on
the **Issues** tab as well as through the API and MCP. Creating and changing reviews and releases is
done through the API, MCP or `liquid review`; the pages read what those calls wrote. The other
writes on the pages are resolving a conflict and retrying a webhook delivery.

## Concepts

| Term | What it is |
|---|---|
| **Change** | A named set of captured writes (`liquid cut`). Reviews, comments and releases point at a change by its id. |
| **Review** | A request to look at exactly one change. Its state is `open`, `approved` or `closed`. |
| **Review comment** | A Markdown comment anchored to a change, a path, a line and a column. When the change is rewritten (for example rebased), the anchor moves with the text. |
| **Issue** | A unit of work or a question, with a Markdown body, labels, assignees, references to changes or paths, and a state of `open` or `closed`. |
| **Build result** | One recorded build action: its outputs, timings, cache outcome and attestations (signed statements about how it was built). The **Artifacts** tab lists them. |
| **Release** | A tag bound to a change and to build results that each carry at least one verified attestation. |
| **Conflict** | Two edits to the same thing that liquid kept side by side instead of guessing. It names the region, explains why, and keeps the base, left and right values, plus a reviewed proposal when one exists. |

Issues, reviews, comments and releases are kept in the repository's own workspace. Every call
below names the repository by `repository_id`; you never pass a workspace.

## Review a Change

1. Open the repository and choose the **Changes** tab, then a change.
2. The page leads with the change's message as its title, then figures (**Files changed**,
   **Lines added**, **Lines removed**, **Checks**), the **Details**, **Review** and
   **Checks and attestations** cards, and every file's differences.
3. Switch the differences between **Unified** and **Side by side**. Comments stay beside their
   line in both layouts. **Copy identifier** copies the full change id.

A change kept on a named review reference opens here before it lands on `main`.

To open a review, comment and approve:

- Open: `POST /v1/create_review` with
  `{"repository_id": "<repository id>", "change_id": "<change id>", "reviewers": ["<reviewer TAAP id>"]}`
  (MCP `create_review`). `reviewers` is optional.
- Comment: `POST /v1/create_review_comment` with
  `{"repository_id": "<repository id>", "review_id": "<review id>", "body": "This should handle an empty file.", "anchor": {"change": "<change id>", "path": "src/parse.cpp", "line": 42, "column": 1}}`
  (MCP `create_review_comment`). The anchor needs all four members.
- Approve or close: `POST /v1/update_review` with
  `{"repository_id": "<repository id>", "review_id": "<review id>", "state": "approved"}`
  (MCP `update_review`). `state` is `open`, `approved` or `closed`.
- Read them: `GET /v1/list_reviews?repository_id=<repository id>` and
  `GET /v1/list_review_comments?repository_id=<repository id>` (MCP `list_reviews`,
  `list_review_comments`).

From a checkout, `liquid review` does the same against Lake: `list`, `show`, `open`, `comment`,
`approve`, `close` and `watch`, with `--lake <repository URL>` ([The liquid CLI](/docs/cli)).

An approved review matters to branch protection: a rule that requires review accepts a push only
when an `approved` review names exactly the change being pushed
([Branch Protection](/docs/repository-settings#branch-protection)).

## Track Work with Issues

1. Open the repository's **Issues** tab. Figures count **Open**, **Closed**, **Labels** and
   **Unassigned** issues.
2. The **Open**, **Closed** and **All** tabs choose the state; the **Label** and **Assignee** chips
   and **Search issues…** narrow the list. The tab and the chips stay in the address, so the link
   can be shared.
3. Open an issue for its description and conversation (Markdown), its labels, assignees and the
   changes and paths it references.

Each action in the web app is the same call as the API and MCP, and needs the same permission
(`write` on the repository). Without it, the buttons stay disabled and say why.

| To | In the web app | REST | MCP |
|---|---|---|---|
| Open an issue | **Issues** → **New issue**: a **Title**, a **Description** (Markdown; **Preview** shows it rendered), **Labels** (choose one the repository uses or type a new one and press **Add**) and **Assignees** (people of the repository's workspace) → **Open issue**. The new issue opens. | `POST /v1/create_issue` | `create_issue` |
| Comment | On the issue, type under **Add a Comment** → **Comment** | `POST /v1/create_issue_comment` | `create_issue_comment` |
| Edit the title or description | **Edit** in the header → change them → **Save** | `POST /v1/update_issue` with `title`, `body` | `update_issue` |
| Change labels | The pencil on **Labels** → toggle or add labels → **Save** | `POST /v1/update_issue` with `labels` | `update_issue` |
| Change assignees | The pencil on **Assignees** → tick the people → **Save** | `POST /v1/update_issue` with `assignees` | `update_issue` |
| Close or reopen | **Close issue** or **Reopen issue** under **State**; with text in the comment box, **Close with comment** posts the comment first | `POST /v1/update_issue` with `state` | `update_issue` |
| See who can be assigned | The **Assignees** list of the drawer or the side card | `GET /v1/issue_assignees?repository_id=<repository id>` | `issue_assignees` |

The calls' bodies:

- Create: `POST /v1/create_issue` with
  `{"repository_id": "<repository id>", "title": "Import fails on an empty CSV", "body": "Steps to reproduce: …", "labels": ["bug"], "assignees": ["<assignee TAAP id>"], "references": [{"path": "src/import.cpp"}]}`
  (MCP `create_issue`). Only `title` is required with `repository_id`. A reference is an object
  naming a `change` or a `path`. Lake records you as the issue's `author`.
- Comment: `POST /v1/create_issue_comment` with
  `{"repository_id": "<repository id>", "issue_id": "<issue id>", "body": "Fixed by the change that adds the empty-file check."}`
  (MCP `create_issue_comment`). Lake records you as the comment's `author`.
- Close or edit: `POST /v1/update_issue` with
  `{"repository_id": "<repository id>", "issue_id": "<issue id>", "state": "closed"}`
  (MCP `update_issue`). The same call changes `title`, `body`, `labels`, `assignees` or
  `references`; send only what changes.
- People to assign: `GET /v1/issue_assignees?repository_id=<repository id>` (MCP
  `issue_assignees`) answers `people`, each with the `taap_id` that `assignees` takes, a
  display name, email and handle.
- Read: `GET /v1/list_issues?repository_id=<repository id>` and
  `GET /v1/list_issue_comments?repository_id=<repository id>` (MCP `list_issues`,
  `list_issue_comments`). Lists are newest first; `limit` caps them.

Two people updating different fields of one issue at the same time both keep their change.

## Resolve a Conflict

When two edits to the same file or folder cannot be merged safely, liquid keeps both and records a
conflict instead of picking one.

1. Open the repository's **Files** tab, then the folder menu → **Conflicts**. (The address is
   `/conflicts?repository_id=<repository id>`.)
2. Each conflict names its kind in words (*Text overlap*, *Structural overlap*, *Suspicious
   concurrent update*, *Suspicious merge proposal*), the path and the exact region, and explains
   **Why This Needs Attention**, with the base, left and right values as evidence.
3. Under **Choose the Value to Keep**, press **Keep left value**, **Keep right value** or, when
   there is one, **Accept reviewed proposal**. Or type the merged result in **Prepared replacement
   text** and press **Use prepared text**.

The resolution is recorded as a new change; nothing earlier is lost.

- List: `GET /v1/list_conflicts?id=<repository id>` (MCP `list_conflicts`).
- Resolve: `POST /v1/resolve_conflict` with
  `{"repository_id": "<repository id>", "ref": "main", "path": "notes/plan.md", "conflict_id": "<conflict id>", "resolution": "left"}`
  (MCP `resolve_conflict`). `resolution` is `left`, `right`, `accept_proposal` or `blob`; with
  `blob`, add `blob` with the replacement bytes in base64. The `path` must be inside the
  conflict's region.

From a checkout, `liquid conflicts` lists them and `liquid conflict resolve <path>` resolves one.

## Inspect Build Results

The **Artifacts** tab (titled **Build Results**) lists the build actions recorded in the
repository, with figures for cache hits, failures and divergent outputs. Filter by action, output
path or content digest. Open a result to see its outputs, timing, cache and divergence state, the
attestations Lake verified, the trace it belongs to, and **Search inside** for text and source
members of its outputs.

- List: `GET /v1/list_artifacts?id=<repository id>&limit=100` (MCP `list_artifacts`). Filters:
  `action`, `output`, `content`; follow `next_cursor` with `cursor` until `done` is true.
- One result: `GET /v1/get_artifact?id=<repository id>&result_id=<result id>` (MCP
  `get_artifact`).

Build results are written by liquid's build (`liquid wrap`, `liquid build`) and pushed with the
repository; Lake verifies their attestations when it reads them. A change's **Checks and
attestations** card shows the same results for that change.

## Publish a Release

A release binds a tag and a change to build results. Lake refuses a release unless every chosen
result has at least one verified attestation, so a release is always backed by evidence.

1. Find the ids of the build results to include on the **Artifacts** tab (or with
   `list_artifacts`).
2. Create the release: `POST /v1/create_release` with
   `{"repository_id": "<repository id>", "tag": "v1.4.0", "change_id": "<change id>", "artifact_ids": ["<result id>"]}`
   (MCP `create_release`).
3. Open the **Releases** tab: each release shows its tag, change, artifacts, output counts and
   verified attestation counts. The repository home's **Releases** card shows the latest.

Read releases with `GET /v1/list_releases?repository_id=<repository id>` (MCP `list_releases`).

## Limits

- Issues, reviews, comments and releases live in the repository's workspace: while it is not ready
  they cannot be read or written, and they are deleted with it.
- There is no single-issue read; `list_issues` returns them all and the page finds one in the list.
- Labels are free text on each issue. Lake keeps no list of labels or their colours; the web app
  colours a label from its name.
- Issues and comments written before lake#598 have no recorded author.
- A review covers exactly one change. Approving an ancestor or a different change does not satisfy
  a review rule.
- Build-result pages hold 1 to 1,000 results (100 by default).
- Outputs nested more than one container deep are listed by digest but not searchable.

## Permissions

| To | You need |
|---|---|
| Read issues, reviews, comments and releases of a public repository | nothing; signed out works |
| Read them on a private or organization repository | `read` on the repository |
| Create or update issues, reviews, comments and releases | `write` on the repository |
| List the people an issue can be assigned to | `write` on the repository |
| Resolve a conflict | `write` on the repository |

## Troubleshooting

| You see | Why, and what to do |
|---|---|
| *Reviews are unavailable* on a change, or *Releases are unavailable* on the home page | The repository's workspace cannot be read right now (for example while it starts). Everything else on the page still works; reload once the workspace is ready. |
| `workspace_not_ready` | Same cause, from the API. Retry when the workspace is ready. |
| `conflict` at `/repository_id` | The repository has no workspace yet; an operator must assign one before it can hold issues or reviews. |
| `not_found` at `/issue_id` or `/review_id` | The id belongs to another repository, is of another kind, or is not a UUID. |
| `create_release` refused | A chosen build result has no verified attestation. Rebuild with liquid so the result is attested, or leave it out. |
| `invalid_input` at `/resolution` | The value is not one of `left`, `right`, `accept_proposal` or `blob`, or `blob` is missing its bytes. |
| **New issue**, **Comment**, **Edit** or **Close issue** is disabled | Its tooltip says why: you are signed out, or you can read the repository but not write it. Sign in, or ask its owner for a `write` grant ([Grants and Effective Access](/docs/access)). |
| An assignee shows as a shortened identity | Names come from the repository's people, which only someone with `write` can list; the identity is the TAAP id the issue holds. |
| `unauthorized` | You can read the repository but not write it. Ask its owner for a `write` grant ([Grants and Effective Access](/docs/access)). |

## Related Pages

- [Repositories](/docs/repositories)
- [Branch Protection, Webhooks, Import and Export](/docs/repository-settings)
- [The liquid CLI](/docs/cli)
- [Search](/docs/search)
- [Grants and Effective Access](/docs/access)
