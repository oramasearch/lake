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
| Conflicts | `GET /conflicts?repository_id=<id>` | `list_conflicts` | unresolved conflict paths and their blob values |
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

The change view groups those values into a forge review. Unified text is the server-rendered
default, while one small script switches to an already-rendered side-by-side representation and
copies the full change identity. Comments remain bound to their liquid text anchor. Review state,
per-file statistics, check outcomes, and verified attestation counts are readable without that
script.

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
