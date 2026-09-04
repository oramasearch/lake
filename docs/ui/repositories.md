# Repository pages

The repository UI is rendered on the server by the pure functions declared in
`include/lake/api/web_ui.hpp`. It has no client-side code. Each route calls the same registry
capability as REST and MCP, so authorization and audit recording have one path.

| Page | Route | Capability | Contents |
|---|---|---|---|
| Repository list | `GET /repositories` | `list_repositories` | repositories the caller may open |
| Repository | `GET /repository?id=<id>` | `get_repository`, `repository_refs` | metadata and published refs |
| Tree | `GET /repository/tree?id=<id>&ref=<ref>` | `repository_tree` | paths, kinds, blobs and modes at the ref |
| File | `GET /file?repository_id=<id>&ref=<ref>&path=<path>` | `repository_file` | a file, its backing blob and escaped contents |
| Change log | `GET /changes?repository_id=<id>` | `list_changes` | libliquid change identities and metadata |
| Change | `GET /change?repository_id=<id>&id=<change>` | `get_change` | change identity and metadata; structural diff is deferred to liquid #166 |
| Conflicts | `GET /conflicts?repository_id=<id>` | `list_conflicts` | unresolved conflict paths and their blob values |
| Artifact | `GET /artifact?repository_id=<id>&id=<result>` | `get_artifact` | result identity; records and attestations are deferred to liquid #166 |
| Repository access | `GET /repository/access?id=<id>` | `entity_effective_access` | direct and inherited effective access |

## Anonymous public read

Unauthenticated calls may list and open only repositories whose visibility is `public`. A private
or organization repository is omitted from the list and its direct page redirects to `/login`.
The same rule covers repository refs, trees, files, logs, changes, conflict values and the artifact
placeholder. Signed-in calls use the policy engine instead.

## Deferred data

The change page intentionally has no diff output until liquid #166 exposes a public structural
diff API. The artifact page intentionally has no record or attestation output until that issue
exposes public enumeration for results, provenance, reproducibility, traces and SBOMs. Both pages
render the requested identity now, making the later extension localized and visible to users.
