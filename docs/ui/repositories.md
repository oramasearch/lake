# Repository pages

The repository UI is rendered on the server by the pure functions declared in
`include/lake/api/web_ui.hpp`. Each route calls the same registry
capability as REST and MCP, so authorization and audit recording have one path.

| Page | Route | Capability | Contents |
|---|---|---|---|
| Repository list | `GET /repositories` | `list_repositories` | repositories the caller may open |
| Repository | `GET /repository?id=<id>` | `get_repository`, repository content, forge release, and effective-access reads | tree, recent changes, rendered README, refs and releases, clone/push commands, indexed language and size totals, lifecycle, and access |
| Tree | `GET /repository/tree?id=<id>&ref=<ref>` | `repository_tree` | paths, kinds, blobs, modes, stable nodes, and sizes at the ref |
| File | `GET /file?repository_id=<id>&ref=<ref>&path=<path>` | `repository_file`, `list_changes`, `change_diff` | highlighted or plain source, line/range anchors, blame, backing blob, and raw/download links |
| Change log | `GET /changes?repository_id=<id>` | `list_changes` | libliquid change identities and metadata |
| Change | `GET /change?repository_id=<id>&id=<change>` | `get_change`, `change_diff`, `list_reviews`, `list_review_comments`, `list_artifacts` | review title/message, compact identities, state, file stats, text-hunk views, anchored comments, conflict values, checks, and attestations |
| Issues | `GET /issues?repository_id=<id>` | `list_issues` | issue state, labels and assignees with server-side query filtering |
| Issue | `GET /issue?repository_id=<id>&id=<issue>` | `list_issues`, `list_issue_comments` | sanitized issue body and discussion, labels, assignees, and linked change identities |
| Releases | `GET /releases?repository_id=<id>` | `list_releases` | sanitized notes, tag and change identity, artifact output and verified-attestation counts |
| Webhooks | `GET /webhooks?repository_id=<id>` | `list_webhooks`, `list_webhook_deliveries`, `retry_webhook_delivery` | endpoint subscriptions plus durable delivery attempts and failed-attempt retry |
| Branch protection | `GET /protection?repository_id=<id>` | `list_branch_protections`, `audit` | ref rules and their signed denied-update audit records |
| Conflicts | `GET /conflicts?repository_id=<id>` | `list_conflicts`, `resolve_conflict` | unresolved conflict kind, region, explanation, reasons, values, and write-authorized resolution controls |
| Artifacts | `GET /artifacts?repository_id=<id>` | `list_artifacts` | filtered cursor pages of libliquid build results |
| Artifact | `GET /artifact?repository_id=<id>&id=<result>` | `get_artifact` | one result, outputs, timing, cache/divergence state, verified attestations and trace summary |
| Repository access | `GET /repository/access?id=<id>` | `entity_effective_access` | direct and inherited effective access |

## Anonymous public read

Unauthenticated calls may list and open only repositories whose visibility is `public`. A private
or organization repository is omitted from the list and its direct page redirects to `/login`.
The same rule covers repository refs, trees, files, logs, changes, conflict values and the artifact
placeholder. Signed-in calls use the policy engine instead.

## Change diffs

`change_diff` calls libliquid's public `repository::diff` with text hunks enabled. Each path keeps
its added, removed, modified, moved, mode-changed, and conflicted status; old and new modes and blob
digests remain visible. Text hunks include their old/new ranges and escaped removed/added lines.
Conflict digests are values on the path instead of an error state. A change with no visible hunks
renders the explicit empty state.

The conflicts page leads with plain conflict information rather than a digest list. Each card names
the kind in person-facing words, the repository path and exact byte or stable-node region, Liquid's
explanation, and every reason a suspicious proposal needs attention. Base, left, right, and optional
proposal identities remain available as supporting evidence. Signed-in writers can keep either
side, accept an available reviewed proposal, or supply prepared replacement text; the form calls
`resolve_conflict` and the registry applies path authorization and signed audit.

The change view groups those values into a forge review. Unified text is the server-rendered
default, while one small script switches to an already-rendered side-by-side representation and
copies the full change identity. Comments remain bound to their liquid text anchor. Review state,
per-file statistics, check outcomes, and verified attestation counts are readable without that
script.

Issue list and detail routes likewise stay adapters over forge capabilities. State, label, and
assignee filters are applied to the authorized list; detail lookup and comment association use
their durable record identities. Bodies and comments share the server-side Markdown renderer, so
stored raw HTML is displayed as text and never reaches the response as markup.

Release, webhook, and protection pages also receive only capability results. Release records keep
their artifact and verified-attestation evidence beside safe notes. Webhook history never receives
the stored signing secret and retries create a new durable attempt. The protection page joins its
rule list with signed refusal events by repository identity; it does not infer denials from the
current rule set.

The file browser resolves each path's newest change from those structural diffs. File pages map
text-hunk ranges onto numbered source lines for blame. C and C++ source is parsed with liquid's
tree-sitter grammar target before the renderer adds syntax roles; unknown extensions remain
escaped plain text. The range script enhances ordinary line anchors and does not fetch data.

## Artifact results

`list_artifacts` maps the action, output path, content digest, cursor, and page-size filters to
libliquid's `repository::results`. The returned cursor is opaque and the next-page link preserves
the active filters. Lake authorizes the repository object and marks this candidate-returning list
for the registry's batched-filter invariant before opening the customer repository.

`get_artifact` loads one result and renders its outputs, phase timings, cache and divergence state,
worker and timestamps. Provenance is verified against the signed result and action. Direct result
attestations, including reproducibility, use the registered TAAP profile verifier. Each SBOM report
keeps libliquid's verification result. The page finds the retained build trace containing the
result and summarizes wall time, action/dependency counts, and cache hits/misses. Missing trace or
attestation data is an explicit empty state.

## First repository

Signed-in visitors with visible customers get the `POST /repository/create` form on
`/repositories`. It collects the workspace, repository name, and description; new repositories
are private. The workspace choice lists each live workspace of the viewer's spaces, grouped by
space, with the remembered workspace preselected; the workspace names its space, so the form sends
only `workspace_id`. A space without a workspace is sent to `/workspaces` first: a repository
belongs to one workspace and there is no default (lake#315). The adapter calls `create_repository`
through the registry and redirects to the repository home. Authorization checks the space even
when a request bypasses the form, and a `customer_id` that is not the workspace's space is refused.
Password sign-up supplies the personal customer and inherited ownership before this page.
See [sign-up-walkthrough.md](sign-up-walkthrough.md).

## The owning workspace

Every repository belongs to one workspace (lake#315, [repositories.md](../api/repositories.md)).
The repository pages show it; they do not move under `/w/<workspace id>/`.

- The repository list has a **Workspace** column linking to `/w/<id>/design/projects` for a
  workspace of the viewer's spaces, and "Needs a workspace" for a repository migration `0051` could
  not assign.
- The repository home names its workspace beside the heading, with the same link, for a signed-in
  viewer who may read the space (`get_workspace`); an anonymous or outside reader of a public
  repository is not told the space's workspace names. Its **Build image** form carries the
  repository's `workspace_id`, which `build_image` requires.

Why not `/w/<id>/`: the workspace navigation in
[workspace-data-plane.md](../architecture/workspace-data-plane.md) makes the path authoritative for
Design, Train and Ship pages, where the person chooses the workspace and the page lists that
workspace's objects. A repository page is addressed by the repository itself, which already names
its workspace and never changes it, so a workspace segment would only repeat it or contradict it.
The same URLs are the public, anonymous addresses of public repositories, the clone and push
addresses, and the links in webhooks, issues and releases; readers of those have no workspace
selector, and a workspace in them would break each link if it were ever wrong. Workspace pages that
list repositories (the Ship → Apps source choice) do sit under `/w/<id>/`, and REST and MCP calls
under `/w/<id>/` narrow `list_repositories` to that workspace.
