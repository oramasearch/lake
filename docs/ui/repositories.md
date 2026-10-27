# Repository pages

The repository pages are web-app pages in `frontend/src/pages/repositories/` (the platform pages in
`frontend/src/pages/apps/`). Each reads the registry capability REST and MCP callers use, so
authorization and audit recording have one path. Their presentation rules are in
[forge.md](forge.md).

| Page | Route | File | Capabilities | Contents |
|---|---|---|---|---|
| Repository list | `/repositories` | `RepositoriesPage.tsx` | `list_repositories`, `create_repository` | repositories the caller may open with their figures, search and workspace and visibility filters, and New repository (a drawer) |
| Repository | `/repository?id=<id>` | `RepositoryPage.tsx` | `get_repository`, `repository_tree`, `repository_refs`, `repository_file`, `list_changes`, `list_releases`, `get_customer`, `get_workspace`, `entity_effective_access`, `build_image` | tree, recent changes, README, refs and releases, clone/push commands, language and size totals, lifecycle, workspace and access |
| Tree | `/repository/tree?id=<id>&ref=<ref>` | `TreePage.tsx` | `repository_tree`, `list_changes`, `change_diff` | paths, kinds, modes and sizes at the ref, with each path's newest change |
| File | `/file?repository_id=<id>&ref=<ref>&path=<path>` | `FilePage.tsx` | `repository_file`, `list_changes`, `change_diff` | highlighted or plain source, line/range anchors, blame, backing blob, raw/download links |
| Change log | `/changes?repository_id=<id>` | `ChangesPage.tsx` | `list_changes` | change identities and metadata |
| Change | `/change?repository_id=<id>&id=<change>` | `ChangePage.tsx` | `get_change`, `change_diff`, `list_reviews`, `list_review_comments`, `list_artifacts` | title and message, compact identities, state, file stats, text hunks, anchored comments, conflict values, checks and attestations |
| Issues | `/issues?repository_id=<id>` | `IssuesPage.tsx`, `NewIssue.tsx` | `list_issues`, `my_permissions`, `issue_assignees`, `create_issue` | Open, Closed and All tabs, label and assignee chips and search, filtered in the page; New issue (a drawer) |
| Issue | `/issue?repository_id=<id>&id=<issue>` | `IssuePage.tsx` | `list_issues`, `list_issue_comments`, `my_permissions`, `issue_assignees`, `update_issue`, `create_issue_comment` | the description and conversation with a comment box, editing, labels, assignees, close and reopen, references |
| Releases | `/releases?repository_id=<id>` | `ReleasesPage.tsx` | `list_releases` | notes, tag and change identity, artifact outputs and verified attestations |
| Webhooks | `/webhooks?repository_id=<id>` | `WebhooksPage.tsx` | `list_webhooks`, `list_webhook_deliveries`, `retry_webhook_delivery` | subscriptions, durable delivery attempts, failed-attempt retry |
| Branch protection | `/protection?repository_id=<id>` | `ProtectionPage.tsx` | `list_branch_protections`, `audit` | ref rules and their signed refused-update records |
| Conflicts | `/conflicts?repository_id=<id>` | `ConflictsPage.tsx` | `list_conflicts`, `resolve_conflict` | conflict kind, region, explanation, reasons, values, resolution |
| Artifacts | `/artifacts?repository_id=<id>` | `ArtifactsPage.tsx` | `list_artifacts` | filtered cursor pages of build results |
| Artifact | `/artifact?repository_id=<id>&id=<result>` | `ArtifactPage.tsx` | `get_artifact` | one result, outputs, timing, cache/divergence, attestations, trace summary |
| Repository access | `/repository/access?id=<id>` | `../organizations/AccessPage.tsx` | `entity_effective_access` | direct and inherited effective access |
| Environments, Environment, Logs, Export | `/environments`, `/environment`, `/logs`, `/exports` | `../apps/EnvironmentsPage.tsx`, `EnvironmentPage.tsx`, `LogsPage.tsx`, `ExportsPage.tsx` | `list_environments`, `get_environment`, `list_deployments`, `create_domain`, `delete_domain`, `environment_logs`, `list_exports` | the repository's environments, one environment's deployments and domains, its logs, its signed exports |

A repository file is edited from its Drive preview (`/drive/preview`): **Edit**, a monospace editor,
**Commit** (and **Commit and redeploy** for a file in an App's source), with a stale file refused as a
conflict. See [drive.md](drive.md#editing-a-file) (lake#1239).

## Anonymous public read

A signed-out visitor sees only repositories whose visibility is `public`, in the list and by
address. A private or organization repository is omitted from the list, and opening its address
sends the visitor to `/sign-in?next=<address>` without saying whether it exists. The same rule covers refs, trees, files, logs,
changes, conflict values and artifacts. Signed-in calls use the policy engine.

## Change diffs

`change_diff` calls libliquid's public `repository::diff` with text hunks enabled. Each path keeps
its added, removed, modified, moved, mode-changed and conflicted status; old and new modes and blob
digests stay visible. Text hunks include their old/new ranges and removed/added lines. Conflict
digests are values on the path instead of an error state. A change with no visible hunks shows the
explicit empty state.

The conflicts page leads with plain conflict information rather than a digest list: the kind in
person-facing words, the path and exact byte or stable-node region, Liquid's explanation, and every
reason a suspicious proposal needs attention, with the base, left, right and proposal identities as
evidence. A signed-in writer can keep either side, accept an available reviewed proposal, or supply
prepared replacement text; `resolve_conflict` applies path authorization and signed audit.

Release, webhook and protection pages also read only capability results. Webhook history never
receives the stored signing secret, and a retry creates a new durable attempt. The protection page
joins its rule list with signed refusal events by repository; it does not infer denials from the
current rules. **Every signed-in reader of the repository sees the Protection tab and its rules**
(lake#777); it used to be the operator's alone, because the page read the instance audit and failed
for anyone else. The refusals come from `audit` on the repository's workspace, which the account's
administrators may read (lake#18, PR https://github.com/oramasearch/lake/pull/897) as well as the
operator; to anyone else the page shows the rules and says the administrators can see the
refusals, instead of failing.

Forge records live in the workspace Zetta of the repository's workspace (lake#341,
[forge.md](../api/forge.md#where-forge-records-live)), so these pages keep their addresses and read
them through the same capabilities. The repository home and the change view do not wait for that
workspace: when it cannot be read (for example while it is starting), the home page says
*Releases are unavailable* and the change view *Reviews are unavailable*, with the reason, and show
everything else. The pages that exist only to show forge records show the workspace's error.

The file and tree pages resolve each path's newest change from those structural diffs, one
`change_diff` per change, a few at a time. File pages map text-hunk ranges onto numbered lines for
blame.

A File page shows `repository_file`'s `contents`, the file's UTF-8 text exactly (emoji, `·` and a
byte-order mark included). A file that is not UTF-8 has no `contents` and `binary: true`; its page
says it is not UTF-8 text and offers Raw and Download, which send the exact bytes (lake#1238,
[docs/api/repositories.md](../api/repositories.md#file-contents)).

## Artifact results

`list_artifacts` maps the action, output path, content digest, cursor and page-size filters to
libliquid's `repository::results`. The returned cursor is opaque and the next-page link keeps the
active filters. Lake authorizes the repository object and marks this list for the registry's
batched-filter invariant before opening the repository.

`get_artifact` loads one result: its outputs, phase timings, cache and divergence state, worker
and timestamps. Provenance is verified against the signed result and action. Direct result
attestations, including reproducibility, use the registered TAAP profile verifier. Each SBOM report
keeps libliquid's verification result. The page summarizes the retained build trace containing
the result: wall time, action and dependency counts, cache hits and misses. Missing trace or
attestation data is an explicit empty state.

## First repository

A signed-in viewer with an account gets **Create a Repository** on `/repositories`. It asks for the
workspace, the repository name and an optional description; new repositories are private. The
workspace choice lists each live workspace of the viewer's accounts, grouped by account, with the
current workspace preselected; the workspace names its account, so the call sends `workspace_id`
(and its `customer_id`). An account without a workspace is told to create one on Workspaces first: a
repository belongs to one workspace and there is no default (lake#315). `create_repository` then
opens the repository home. The description is written to the chosen workspace's Zetta (lake#315
part 2), so the create waits for that workspace and is refused while it is not ready. The list and
the home page read descriptions there; a workspace that cannot be read shows *Unavailable* in the
list and *Description unavailable: …* on the home page instead of failing the page. Authorization
checks the account even when a call bypasses the page, and a `customer_id` that is not the
workspace's account is refused. Password sign-up supplies the personal account and its ownership before
this page; see [sign-up-walkthrough.md](sign-up-walkthrough.md).

## The owning workspace

Every repository belongs to one workspace (lake#315, [repositories.md](../api/repositories.md)).
The repository pages show it; they do not move under `/w/<workspace id>/`.

- The repository list has a **Workspace** column linking to `/w/<id>/design/projects` for a
  workspace of the viewer's accounts, and "Needs a workspace" for a repository migration `0051` could
  not assign.
- The repository home names its workspace beside the heading, with the same link, for a signed-in
  viewer who may read the account (`get_workspace`); an anonymous or outside reader of a public
  repository is not told the account's workspace names. **Build image** sends the repository's
  `workspace_id`, which `build_image` requires, and opens that workspace's Packages page, where
  the build shows.

Why not `/w/<id>/`: the workspace navigation in
[workspace-data-plane.md](../architecture/workspace-data-plane.md) makes the path authoritative for
Design, Train and Ship pages, where the person chooses the workspace and the page lists that
workspace's objects ([workspace-selection.md](workspace-selection.md)). A repository page is addressed by the repository itself, which already names
its workspace and never changes it, so a workspace segment would only repeat it or contradict it.
The same URLs are the public, anonymous addresses of public repositories, the clone and push
addresses, and the links in webhooks, issues and releases; readers of those have no workspace
selector, and a workspace in them would break each link if it were ever wrong. Workspace pages that
list repositories (the Ship → Apps source choice) do sit under `/w/<id>/`, and REST and MCP calls
under `/w/<id>/` narrow `list_repositories` to that workspace.
