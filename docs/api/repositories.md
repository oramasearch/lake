# Repositories

A repository is owned by a customer (the tenant) and named within it: the address is
`<customer handle>/<name>`. Schema: [`../control-plane/schema.md`](../control-plane/schema.md)
(migrations `0002_repositories`, `0006_repository_lifecycle`,
`0040_active_repository_names`, which limits name uniqueness to live rows, and
`0051_repository_workspaces`, which gives every repository its workspace, and
`0064_repository_records`, which moves its description into that workspace and drops
`repository_refs`).

Every repository belongs to one workspace of its account (lake#315, part 1: routing). The
workspace is named on create and never changes; there is no default workspace. The repository row
in lake's PostgreSQL is the routing row (S10): id, account, workspace, name, visibility
and tombstone, which lake needs to find a repository by name, serve an anonymous read of a public
one and collect expired tombstones before it reads any workspace. Its description is a record in
that workspace's Zetta ([below](#where-the-description-lives-lake315-part-2)); forge records moved
with lake#341, and its content moves with lake#380.

| Field | |
|---|---|
| `id` | uuid v4 |
| `customer_id` | the owning customer; deleting the customer deletes its repositories |
| `workspace_id` | the workspace of that customer the repository belongs to; set on create and never changed. `null` only for a repository created before workspaces that migration `0051` could not assign (see below) |
| `name` | slug, same format as the handle (lowercase, digits, inner hyphens, 1–256); unique among live repositories in the customer |
| `description` | free text, default empty; the repository's settings record in its workspace (lake#315 part 2) |
| `description_unavailable` | only in answers whose description could not be read from the workspace (no workspace yet, or the workspace refused, for example while it starts): the reason; `description` is then empty |
| `visibility` | `private` (default), `organization`, or `public` |
| `deleted_at`, `retention_until` | null while live; deletion tombstones the repository for 30 days before its customer store may collect unreferenced data |
| `created_at`, `updated_at` | timestamps |

| Capability | Kind | Input | Errors |
|---|---|---|---|
| `list_repositories` | read | `customer_id?` (narrows to one tenant), `workspace_id?` (narrows to one workspace), `limit?` | — (a workspace that cannot be read marks its rows `description_unavailable`) |
| `get_repository` | read | `id` | `404 not_found` |
| `create_repository` | action | `customer_id`, `workspace_id`, `name`, `description?`, `visibility?` | `400 invalid_input` at `/workspace_id` (missing, unknown or deleted workspace), at `/customer_id` (another account's workspace, or an unknown customer), bad name, bad visibility; `409 conflict` (name taken within the customer); with a description, `workspace_not_ready` while the workspace cannot be written, and nothing is created |
| `rename_repository` | action | `id`, `name` | `404 not_found`, `409 conflict` (tombstoned or name taken) |
| `transfer_repository` | action | `id`, `customer_id` | always `409 conflict` at `/customer_id`: a repository stays in its account and workspace; export it and import it into the other account's workspace. `404 not_found` |
| `set_repository_visibility` | action, `admin` on the repository | `id`, `visibility` | `400 invalid_input`, `401 unauthorized` (a caller without `admin`, a writer included), `404 not_found`, `409 conflict` (tombstoned) |
| `delete_repository` | action | `id` | `404 not_found`, `409 conflict` (already tombstoned) |
| `repository_refs` | read | `id` | `{items: [{name, change}]}`, from the liquid repository |
| `repository_tree` | read | `id`, `ref` | `{items: [{path, kind, blob, mode, node_id, size, stored_size}]}` |
| `repository_file` | read | `id`, `ref`, `path`, `units?` | `{path, blob, contents?, content_base64, binary, size, units?}`: see [File contents](#file-contents) |
| `list_changes` | read | `id` | `{items: [{id, parents, author, message, occurred_at}], people}`: see [Signers by name](#signers-by-name) |
| `get_change` | read | `id`, `change_id` | change identity and metadata, and `people` for its author |
| `change_diff` | read | `id`, `change_id` | `{items: [{path, old_path, kind, changes, old_mode, new_mode, old_blob, new_blob, line_hunks, old_conflicts, new_conflicts}]}` |
| `list_conflicts` | read | `id` | `{items: [{id, base, left, right, region, kind, explanation, proposal?, reasons, node?}]}` |
| `resolve_conflict` | action | `repository_id`, `ref`, `path`, `conflict_id`, `resolution`, `blob?` | `{conflict, value, change}` |
| `list_artifacts` | read | `id`, `action?`, `output?`, `content?`, `cursor?`, `limit?` | `{items: [result], next_cursor, done}` |
| `get_artifact` | read | `id`, `result_id` | one result with outputs, timings, cache/divergence state, attestations and trace |
| `refs` | read | `repository_id` | `{items: [{repository_id, name, head}]}` from the liquid repository (no `updated_at`: lake keeps no copy of refs since `0064`, lake#319); `404 not_found`, `409 conflict` (tombstoned) |
| repository sync routes | binary read/action | repository URL and liquid protocol body | liquid protocol errors; see [`sync.md`](sync.md) |
| `audit` | read | audit filters | [`audit.md`](audit.md), including the lifecycle capability calls |

Every lifecycle capability carries the repository or customer authorization object and its
`write` action. `Registry::call` invokes `authorize(principal, object, action)` before the
handler and appends its signed outcome to `audit_events` through `audit::record`.

`list_repositories` resolves every candidate repository policy object and makes one
`authorize_batch(..., "read", ...)` call before returning the permitted set. Repository content
capabilities authorize the repository before opening its libliquid data. The other candidate
lists follow their policy-object or scoped parent authorization paths, and the registry records
every successful or denied call.

## Repository data and change pages

`repository_refs`, `repository_tree`, `repository_file`, `list_changes`, and `list_conflicts`
read libliquid directly through the lake repository-store adapter. Tree output is recursive;
every entry retains its stable node identity plus logical and stored sizes, and every file links
to its backing blob identity and its contents ([File contents](#file-contents)).

A folder or file below a path object that does not inherit is read only with a grant there, as in
Drive (lake#975): `repository_file` answers `unauthorized`, `repository_tree`, `change_diff` and
`list_conflicts` leave it out, and `pull`, `read_chunk` and `export_repository`, which carry every
path, are refused unless the caller may read every such folder. See
[policy.md](policy.md#restricted-folders-hold-for-every-repository-read-lake975).

### File contents

`repository_file` answers every file's exact bytes as standard padded base64 in `content_base64`.
`contents` is the file's text, and it is exactly those bytes, when they are well-formed UTF-8
(RFC 3629): emoji, `·`, a byte-order mark, CR and CRLF line endings are all kept as they are, and
`binary` is `false`. A file whose bytes are not UTF-8 has no `contents` field and `binary: true`;
read it from `content_base64`. Before lake#1238, `contents` wrote every byte outside printable
ASCII as the literal text `\xNN` (👥 became `\xF0\x9F\x91\xA5`), so a client that reads text should
read `contents` from a release with this fix, or decode `content_base64` itself.
`resolve_model_file` and `resolve_dataset_file` ([models.md](models.md)) answer `contents` and
`binary` the same way, the console's File page shows a binary file as such, and
`GET /file/raw` and `GET /file/download` send the file's exact bytes (`text/plain; charset=utf-8`
for text, `application/octet-stream` for a binary file).
`DrivesHttp.RepositoryFileContentsAreTheExactUtf8TextOrAbsentForBinary` checks every case.

`repository_file` with `units: true` also answers the file's **syntax units** (lake#561), the
shading the File page draws: `units: [{start_line, end_line, kind}]` from libliquid's
`liquid_frontend::parse_units`, lines as liquid reports them (the page shades `start_line` through
`end_line`), `kind` liquid's numeric unit kind. Only the formats lake parses (C and C++, by
extension: `.c`, `.h`, `.cc`, `.cpp`, `.cxx`, `.hh`, `.hpp`, `.hxx`) carry `units`; any other file,
and any call without `units: true`, has no `units` field. The server-rendered File page and the
capability share `web_ui_source_units`. `Repositories.PublicRepositoryContentCapabilitiesUseTheRepositoryStore`
checks both cases.

`list_conflicts` exposes Liquid's first-class conflict record without reducing it to digests. The
record names its content identity; base, left, and right values; kind (`text`, `structural`,
`crdt-suspicious`, or `suspicious-merge`); repository-relative region with either byte or stable-node
range; plain explanation; optional reviewed proposal; detailed reasons; and optional affected node.
Structural values can reference a tree. Lake retains those canonical digests in the API while its
drive adapter resolves the affected path inside each tree for a two-file comparison.

`resolve_conflict` is the write-authorized resolution action rendered into REST, OpenAPI, and MCP.
`resolution` is `left`, `right`, `accept_proposal`, or `blob`; the last form requires base64 bytes in
`blob`. The request path must match the conflict region so authorization cannot be borrowed from a
different path. Liquid records the chosen value as a `conflict_resolution` in the returned change,
and Lake returns both identities and records the signed audit outcome.

`change_diff` compares the selected change with its first parent through libliquid's public
`repository::diff` API. A root change is compared with an empty tree. The result retains every
per-path status, old/new mode and blob digest, move origin, conflict digest, and optional text hunk.
Lake reads the hunk ranges from their blobs so the API and page show escaped removed and added
lines. A change without visible hunks produces the page's empty state.

## Artifact data

`list_artifacts` maps its filters and opaque cursor directly to libliquid's paged
`repository::results` query. The page size is limited to 1–1000. `get_artifact` loads exactly one
result, its output blob records, action-phase timing, cache/divergence state, worker, and timestamps.
It verifies the linked SLSA provenance against the signed result/action, verifies direct result
attestations such as `reproducibility/1` through the registered TAAP profile, and carries each
`sbom/1` report's verification outcome. It scans retained trace pages for the trace containing the
result and returns its wall time, action/dependency totals, and cache summary.

Both capabilities authorize the repository object before opening its customer data. The result
list is marked as candidate-returning and uses the same batched repository authorization path as
the other lists. Anonymous callers may reach it only for a public repository.

## Public repositories

An anonymous caller may call `list_repositories` and the repository read capabilities only for a
repository marked `public`. The repository list omits every other visibility level. A direct read
of a private or organization repository returns `unauthorized`; the HTML routes turn that result
into a sign-in redirect. A signed-in caller continues through the repository policy object.

Changing who may read is an administrator's decision (lake#1310): `set_repository_visibility`
needs `admin` on the repository, held through a grant on it or inherited from its account (an
owner or administrator of the account), or the operator credential. A `write` grant is not enough:
a writer can push and file issues but is answered `unauthorized`, and the visibility stays as it
was. This matches `set_branch_protection` and `replicate_repository`, which also need `admin`.
`Repositories.OnlyAnAdministratorChangesVisibility` proves a writer is refused and the repository
stays private, and that an administrator of the repository still makes it public.

## Lifecycle

Rename changes only the repository slug. Transfer is refused since migration `0051`: the
routing trigger keeps a repository's account and workspace fixed, so moving one to another account is an
export and an import into a workspace of that account. liquid #160 and #165 publish the sync client,
server, and wire contract. Lake exposes that contract at
`/v1/repositories/<repository-id>/sync/*`; [`sync.md`](sync.md) records the routes, binary bodies,
authorization, and atomic-rejection guarantees.

Delete is a tombstone, not an immediate erase: `deleted_at` and `retention_until` are stored on the
repository record. The collector may reclaim data after the documented 30-day retention period,
but only when no remaining repository root leases it. A tombstoned repository cannot be renamed
or have visibility changed. Deletion releases the customer-scoped name immediately;
creating a new repository with that name creates a new identity and leaves the tombstone intact.

Store diagnostics never enter the REST or MCP envelope. A duplicate live name returns
`conflict: a repository named '<name>' already exists` with `path: /name`; the raw database detail
is written only to the server log. Other database diagnostics are replaced with a product message
at the API boundary.

The repository detail page at `/repository?id=<id>` presents current lifecycle state. The signed
audit page at `/audit` exposes the same capability outcomes. All mutations remain capabilities,
so REST, OpenAPI, and MCP expose the same operations.

```sh
H='Authorization: Bearer <token>'
curl -H "$H" -H 'content-type: application/json' \
  -d '{"customer_id":"<customer id>","workspace_id":"<workspace id>","name":"liquid","description":"the tool","visibility":"public"}' \
  https://lake.example.com/v1/create_repository
curl -H "$H" 'https://lake.example.com/v1/list_repositories?customer_id=<customer id>&limit=20'
curl -H "$H" -H 'content-type: application/json' \
  -d '{"id":"<repository id>","visibility":"organization"}' \
  https://lake.example.com/v1/set_repository_visibility
curl -H "$H" -H 'content-type: application/json' \
  -d '{"id":"<repository id>","name":"liquid-next"}' \
  https://lake.example.com/v1/rename_repository
```

The UI lists them at `/repositories` ([`../ui/README.md`](../ui/README.md)); the detail page
displays the tombstone state. Repository content uses the libliquid customer store. The published
sync contract and lake adapter are documented in [`sync.md`](sync.md). Every repository is opened
with `repository_options.store` over the one caller-owned store at
`LAKE_STORE_ROOT/<customer-id>/store`; it does not get a private `.liquid-store`.

## The owning workspace

`workspace_id` is a content-free routing pointer, like `stores.workspace_id` (migration `0044`) and
`auth_applications.workspace_id` (migration `0049`). It names which workspace Zetta will hold the
repository's metadata, forge records and content when they move.

- **Create.** `create_repository`, `import_repository` and `import_github_repository` require it.
  A missing, malformed, unknown or deleted workspace is `invalid_input` at `/workspace_id`; a
  workspace of another account is `invalid_input` at `/customer_id`, as for every workspace-scoped
  capability. An agent task's credential supplies its task's workspace, and a call under
  `/w/<workspace id>/` (REST or MCP) supplies the path's. An import never reuses the archive's own
  `workspace_id`: the caller names where the copy goes.
- **Immutable.** The `repository_workspace_routing` trigger (`enforce_workspace_routing`) refuses a
  new repository without a live workspace of its own account, and any later change of
  `workspace_id` or `customer_id`, including assigning one to a repository that has none. That is
  why `transfer_repository` is refused. The one exception is migration `0066`'s own block (below).
- **Resolvable from the repository.** `get_repository`, `list_repositories`, the export archive and
  every handler that loads the repository carry it. The push path resolves the repository once and
  hands it, workspace included, to `sync_change_authorizer` and `evaluate_branch_protection`; each
  branch protection audit record and the `push` audit record name the workspace.
- **Workspace deletion.** Deleting a workspace deletes its repositories (lake#514
  S12): in the deletion's transaction every repository of the workspace that is live or still
  inside its retention is tombstoned with its retention ended (`retention_until` now, an earlier
  `deleted_at` kept), which frees its name in the account at once. Each gets a central audit event
  (object `repository/<id>`, request id `workspace-deletion:<workspace_id>`), and its search
  segments are dropped. Their description records (`lake_repositories`) and forge records live in
  the workspace Zetta and go with it. The confirmation lists them first
  ([workspaces.md](workspaces.md#deletion)).
- **Hub clients.** `POST /api/repos/create` takes `workspace_id` in its body or, because
  `huggingface_hub`'s `create_repo` cannot add body fields, the `X-Lake-Workspace-Id` header
  (`HfApi(headers={"X-Lake-Workspace-Id": ...})`).

Migration `0051` assigns each earlier repository, tombstoned ones included, to its account's only live
workspace (`sole_workspace`). A repository of an account with several live workspaces, or none, is
recorded as `needs_selection` in `workspace_assignments` and reported with a `RAISE WARNING`; it
keeps working, with `workspace_id: null`, until an explicitly reviewed migration assigns it. List
them with `SELECT * FROM workspace_assignments WHERE object_table = 'repositories' AND
outcome = 'needs_selection'`.

Migration `0066_assign_unrouted_repositories` is that reviewed migration
(lake#513). It runs once and is generic rather than naming an id: each repository that still has no
workspace, tombstoned ones included, is assigned its account's only live workspace when the account now
has exactly one; any other (several live workspaces, or none) is left alone and reported with a
`RAISE WARNING` naming the repository, the account and the count. Each assignment is recorded in
`workspace_assignments`: the `repositories` row changes from `needs_selection` to `sole_workspace`,
and the `repository_descriptions` row, if `0064` recorded one, to `repository_workspace` (the outcome
`copy_workspace_records` gives a resolved description), with the workspace. The trigger is not
dropped: `enforce_workspace_routing` lets an update change `workspace_id` only on `repositories`,
only from NULL to a live workspace of the same account, and only while the transaction-local setting
`lake.repository_assignment` is `0066_assign_unrouted_repositories`. The block sets it before its
updates and clears it after, so every other change is refused, in the migration's own transaction
and afterwards. Guard `assign_unrouted_repositories_guard`.

On dev this assigns the `liquid` repository (`f4d3d9bf-84fc-4c0c-a39c-952f46cfee04`) to the first
workspace of its account, `d2a67e5d-cf36-4cd6-83a1-2e77625284dd`. Steps, for the coordinator:

1. Have the operator vendor `db/schema.sql` through `0066_assign_unrouted_repositories` and select
   `assign_unrouted_repositories_guard`, then apply it (the workspace may still be Pending). The
   schema Job's log names each assignment (`NOTICE: repository assigned its sole workspace`) and
   each repository left alone (`WARNING: workspace_id requires selection`).
2. Check that `SELECT * FROM workspace_assignments WHERE object_table = 'repositories' AND
   outcome = 'needs_selection'` is empty, and that `get_repository` and the repository page name
   the workspace.
3. Once the workspace is Ready, rerun `copy_workspace_records` for the collection `repositories`
   (`plan`, `copy`, `verify`) so the description is copied into its Zetta; the other collections
   that follow a repository's workspace (forge records, platform) resolve it on their next run.

## Where the description lives (lake#315 part 2)

Lake keeps the repository row central as routing and moves only what a person wrote
about the repository. A repository's description, and any later setting (default branch, topics),
is one sealed record of the `lake_repositories` collection in its workspace's Zetta
([workspace records](../architecture/workspace-records.md)), keyed by the repository id and found by
its account (`customer:<id>`): a workspace belongs to one account, so one `list_records` reads every
repository record that workspace holds. The body names the repository and its account; a record
under another key, in another workspace or of another account does not open.

- **Create.** `create_repository`, `import_repository` and `import_github_repository` with a
  description write the record `if_absent` under a new id first, then the routing row under that
  id; if the row is refused (a taken name), the record is removed. A repository without a
  description has no record. New rows leave the `description` column empty, and the control store
  refuses one.
- **Change.** A change (`update_repository_settings`) reads the record and writes it conditionally
  on the revision read, rereading on a conflict, so concurrent changes are both kept. No capability
  edits a description yet.
- **Read.** `get_repository` reads the record (one `get_record`); `list_repositories` reads one
  `list_records` per listed workspace, all started together. A workspace that cannot be read, or a
  repository with no workspace yet, answers `description: ""` with `description_unavailable` and
  the reason, rather than failing the read. The synchronous `Registry::call`, which lake's own pages
  use to route, answers the routing row alone (no workspace call) and refuses a create with a
  description; REST, MCP and `call_async` read and write the record. The repository page and
  `/repositories` show the description, or say it is unavailable.
- **Deletion.** A deleted workspace takes its records with it; lake#514 cascades workspace
  deletion to its repositories.
- **Copy.** Migration `0064_repository_records` records, in `workspace_assignments` under
  `object_table = 'repository_descriptions'`, the owner of each repository whose row still holds a
  description: `repository_workspace`, `needs_selection` while the repository has no workspace
  (the dev `liquid` repository, until migration `0066` assigns one, lake#513), or
  `workspace_deleted`. The operator's
  `copy_workspace_records` collection `repositories` copies each description with the row's
  creation instant and verifies it; a description changed since (a later lake revision) is
  accepted. The copy was verified on dev, and migration `0070_drop_moved_tables` drops the column
  (lake#311); `copy_workspace_records` no longer accepts `repositories`. Steps:
  [cutover runbook](../architecture/workspace-records.md#cutover-runbook-repository-descriptions-dev-cluster).

`repository_refs`, the table that copied each liquid ref after a push, a replication and a `refs`
read, is dropped by the same migration (lake#319). Nothing read it; the liquid
repository is the only record of a repository's refs.

## Verify

Run `ctest --test-dir build --output-on-failure`. The repository lifecycle tests create two
customers, change visibility, rename, refuse a transfer, tombstone, verify policy denial, and verify the
signed audit stream contains the deletion outcome. Memory and PostgreSQL cases also create,
delete, and recreate the same repository name, and assert duplicate errors contain no database
diagnostics. For Postgres, apply `db/schema.sql` through
the operator’s `PostgresSchema` before running the integration test. `SyncHttp.*` proves push,
incremental chunk transfer, pull, atomic bad-signature rejection, policy denial, and audit records.
`PostgresSync.*` repeats the exchange with the production control store. The repository, drive,
OpenAPI, MCP, and renderer tests verify the complete conflict record and every advertised resolution
kind.

## Change

Lifecycle changes update the applicable migration in `db/schema.sql`, the
byte-identical operator schema asset, this page, `docs/control-plane/schema.md`, and the repository
page renderer in one change. Do not shorten the retention window without a migration and a
documented collection policy.

## Signers by name

**What.** A change's `author` is the TAAP identity that signed it
(`taap:1:ed25519:TY44R4SE…`). The reads that name signers or principals also answer `people`: each
such identity mapped to `{kind, name, handle}` of the account it acts for (lake#1326). `kind` is
`user` or `organization`, `name` the account's display name (possibly empty), and `handle` its
account's name (`@ada`).

| Read | Identities in `people` |
|---|---|
| `list_changes`, `get_change` | each change's `author` |
| `list_reviews` | each review's `opened_by`, `reviewers` and `approval.by` |
| `list_review_comments` | each comment's `author` (a TAAP identifier, or liquid's actor object with `taap_id`) |
| `entity_effective_access` | each entry's `principal_taap_id` |

**How.** `Store::signer_names` resolves an identity the way `Store::account_for_identity` does for
the `push_change` audit: an account's own identifier is itself, and an own identity linked to an
account (lake#762) acts for that account. Then it reads the user's or organization's display name and
its own account's handle. Deleted accounts are left out.

- An identity no account acts for is absent. The web app shows it as **Unknown signer** with a short
  key, the whole key on hover.
- `people` never carries an email. A visitor to a public repository reads names and handles, the
  same as on the repository's owner line.
- If the lookup fails, `people` is empty and the records are still served with their keys.

**Why.** People recognize each other by name. A list of keys makes review harder.

**How to verify.** `Repositories.PublicRepositoryContentCapabilitiesUseTheRepositoryStore`
(a visitor reads the author's name and handle, not the email), `Forge.PersistsIssueLifecycleAndMapsReviewCommentAnchors`
(reviewers by name), `Forge.ARecordsSignersAreItsOpenerReviewersApproverAndAuthor`,
`Repositories.PeopleNameKnownAccountsOnlyAndTheAccessList` (`tests/test_api.cpp`), and
`OwnIdentity.*StoreRotatesToTheOwnIdentity` (both keys of an account with a linked own identity read
as that account, in both stores). The web app's labels: `frontend/tests/signers.test.ts`.

