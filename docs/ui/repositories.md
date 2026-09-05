# Repository pages

The repository UI is rendered on the server by the pure functions declared in
`include/lake/api/web_ui.hpp`. Each route calls the same registry
capability as REST and MCP, so authorization and audit recording have one path.

| Page | Route | Capability | Contents |
|---|---|---|---|
| Repository list | `GET /repositories` | `list_repositories` | repositories the caller may open |
| Repository | `GET /repository?id=<id>` | `get_repository`, repository content, forge release, and effective-access reads | tree, recent changes, rendered README, refs and releases, clone/push commands, indexed language and size totals, lifecycle, and access |
| Tree | `GET /repository/tree?id=<id>&ref=<ref>` | `repository_tree` | paths, kinds, blobs, modes, stable nodes, and sizes at the ref |
| File | `GET /file?repository_id=<id>&ref=<ref>&path=<path>` | `repository_file` | a file, its backing blob and escaped contents |
| Change log | `GET /changes?repository_id=<id>` | `list_changes` | libliquid change identities and metadata |
| Change | `GET /change?repository_id=<id>&id=<change>` | `get_change`, `change_diff` | change identity and its first-parent structural diff, including text hunks |
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
