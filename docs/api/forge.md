# Forge records

**What.** A repository's forge, export, and GitHub-import facts. Each record belongs to a
repository, has a constrained kind, and retains a versioned JSON object as its value. Since
lake#341 every record lives in the workspace Zetta of its repository's workspace
([where they live](#where-forge-records-live)); migration `0012_forge_export_import` created the
retired PostgreSQL table `forge_records`, which only the operator's one-shot copy still reads.

**Why.** A review anchor is an evolving value: it initially holds `{change, path, line, column}`.
When an author rebases a reviewed change as a rewritten candidate, Liquid maps that cursor through
the repository diff and Lake stores the mapped position and disposition without reinterpreting the
original comment.

## Stored kinds

`issue`, `issue_comment`, `review`, `review_comment`, `release`, `webhook`, `webhook_delivery`,
`branch_protection`, `export`, and `github_import` are the allowed values
(`control::forge_record_kinds`). A record of any other kind, or with a value that is not an object,
is refused before it is written.

Each record has a UUID and creation/update timestamps (whole seconds, `YYYY-MM-DDTHH:MM:SSZ`).
Values are intentionally immutable facts where possible; state transitions retain the same record
identifier and replace only its versioned value. Secrets such as webhook signing material are never exposed in a list or export result.

Every forge `list_*` capability is a repository candidate read. It resolves the requested
repository and authorizes its `read` action before returning records. Forge lists therefore remain
authenticated and never turn the public-repository exception into anonymous issue, review, release,
delivery, or policy access.

## Where forge records live

Every forge record is a sealed record of the `lake_forge_records` collection in the workspace
Zetta of its repository's workspace (`repositories.workspace_id`, lake#315), through the
[workspace records](../architecture/workspace-records.md) mechanism: one record per forge record,
keyed by its id, found by the lookup `repository:<id>` + `kind:<kind>`. Listing one kind of one
repository is one `list_records`.

- **The workspace is the repository's.** No forge capability takes a `workspace_id`: the
  repository a caller names has one immutable workspace, so a caller who names the repository has
  named the workspace, and every record of one repository is in one place. Nothing falls back to a
  default workspace, the lake Zetta or PostgreSQL.
- **Ids.** A missing `repository_id` is `invalid_input` at `/repository_id`; an unknown one is
  refused at `/repository_id`. A record id of another repository, of another kind, or not a UUID
  is `not_found` at the field that named it (`/issue_id`, `/review_id`, `/webhook_id`,
  `/delivery_id`).
- **A repository without a workspace** (one migration 0051 recorded as `needs_selection`) has
  nowhere to keep forge records: every forge call on it is `conflict` at `/repository_id` until an
  operator assigns its workspace.
- **A workspace that is not ready** refuses with `workspace_not_ready`; a deleted one with the
  workspace service's refusal. Deleting a workspace deletes its repositories with it (lake#514,
  [workspaces.md](workspaces.md#deletion)), and their forge records go with its Zetta.
- **Immutable ownership.** The sealed value is bound to its workspace, collection, key and lookup,
  so a record copied into another workspace or repository does not open there.
- **Updates** (`update_issue`, `update_review`, `set_branch_protection` on an existing rule,
  `map_review_anchors`) read the record and write it only at the engine revision they read,
  rereading and applying the change again on a conflict. Two updates of different fields both
  survive.
- **Callers.** REST, MCP and the repository pages reach the records only through these
  capabilities; `export_repository`, `import_repository` and `import_github_repository` read and
  write them in the repository's workspace; the push path reads branch protection there
  ([below](#branch-protection)).

## Issues and reviews

`create_issue` creates an open issue with Markdown `body`, labels, assignees, and explicit
references. A reference is an object such as `{ "change": "..." }` or `{ "path": "..." }`.
`update_issue` changes its fields or its state (`open` or `closed`), and `create_issue_comment`
adds Markdown discussion after confirming that the issue belongs to the repository.

`create_review` binds an open review to exactly one liquid `change_id`. `update_review` transitions
that review to `approved` or `closed`. `create_review_comment` verifies that its parent review is
in the same repository and accepts an anchor value with required `change`, `path`, `line`, and
`column` members.

`map_review_anchors` receives a source change, target change, path, and the text hunks between them.
It updates only matching comment anchors and reports `same`, `shifted`, `approximate`, or
`deleted`. `liquid::forge_client::advance_anchors` loads the review comments, derives those hunks
from Liquid's repository diff, persists each mapped cursor through this capability, and retargets
the review to the rewritten change.

Named review references are synchronized with the repository document. Lake's repository history
adapter walks the current head and every named reference, deduplicating change identities, so
`get_change`, `change_diff`, and the change page can resolve an unlanded reviewed change.

## Graph-backed change checks

The change page reads build results through `list_artifacts`. Each listed result carries the same
output, verified attestation, and retained trace evidence as `get_artifact`; the adapter does not
invent a page-only check state. The renderer names every output path and every verified
attestation profile beside the action and result identity.

The end-to-end scenario records a graph-selected action and signed `slsa_provenance/1` statement
without adding automation YAML to the repository. Its final HTML assertions require the output
path, profile, and result identity that Liquid persisted.

## Releases

`create_release` binds a tag and change identity to selected build-result IDs. Lake reads every
result from the repository store and refuses a release unless each result has at least one verified
attestation. The release record copies each selected result's ID, action, output count, and verified
attestation identifiers and subjects. This creates a durable release-to-evidence link even when
build records are later queried independently.

## Webhooks

`create_webhook` stores an event subscription, endpoint, and signing secret. Lake sends events for
issues, reviews, and releases only to subscriptions that name that event or `*`. Each attempt has
canonical JSON payload bytes, an `x-lake-event` header, and an HMAC-SHA-256
`x-lake-signature-256` header. The durable delivery record retains the status, failure text,
signature, payload, and attempt count. `retry_webhook_delivery` creates a new attempt only when a
prior attempt was unsuccessful; it never overwrites the event log.

The sender is an injected service at the API boundary so the registry and its tests do not perform
network I/O. The HTTP service supplies that sender in deployment. If a sender is unavailable, the
attempt is retained as a failed delivery and can be retried when delivery is configured.

## Branch protection

`set_branch_protection` creates or updates one policy rule for an exact ref or a trailing-`*`
prefix. A rule may require an approved review for the proposed change, verified release evidence,
fast-forward-only updates, and a non-empty allow-list of TAAP identities. The rule also owns a
repository-child `path` policy object. A non-operator pusher must be permitted to `write` that
object through `authorize`; inherited repository grants therefore remain effective and ref-specific
grants can narrow access.

Liquid #192 supplies `BranchRefUpdate` values containing the ref name, server-derived prior and
proposed heads, and computed fast-forward result. Lake evaluates every reference callback with the
authenticated caller and returns one accepted or refused verdict per reference. A refusal names the
reference and carries the rule's reason verbatim; for example,
`branch protection rejects a non-fast-forward update to main`. The registry refuses an anonymous
or unauthorized push before invoking the reference evaluator.

The rules, and the reviews or releases a rule asks for, are read from the repository's workspace
once, before the push starts (`load_branch_protection`): the push itself runs synchronously inside
the repository store, so it evaluates that snapshot rather than call the workspace from inside it.
Reviews are read only when a rule requires a review, releases only when a rule requires
verification, so a repository without such rules costs one `list_records` per push. When the
rules cannot be read (the repository has no workspace, or its workspace is not ready or not
reachable), every reference update of the push is refused with that reason, recorded as a
`forge.branch_protection.refused` event with rule `branch_protection_unavailable`; an atomic push
then changes no reference, and an ordinary one applies none of them. A lake configured without any workspace service holds no forge records anywhere, so it
evaluates no rules.

An approved review is a `review` record whose `change_id` exactly equals the reference's proposed
head and whose current `state` is `approved`. Approval for an ancestor, a different change, or a
review in any other state does not satisfy `required_review`.

Lake records one signed audit event for each protected reference decision. Allowed decisions use
kind `forge.branch_protection.allowed`; refusals use `forge.branch_protection.refused`. Both retain
the repository, reference, caller, matched rule, and reason, never a credential.

An ordinary push applies accepted references independently and leaves refused references at their
pre-push heads. The successful reply reports every requested reference as `applied` or `refused`;
refused entries include the exact rule reason. A client that requires all-or-nothing publication
sets `atomic: true`. Any refusal then leaves every reference unchanged, preserving the original
refusal reason and reporting otherwise accepted entries as refused because the atomic push failed.

## Verify

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

The forge capability tests create and read records in a test workspace Zetta.
`Stores/WorkspaceRecordStores.ForgeRecordsLiveInTheirRepositorysWorkspace` covers two spaces with
two workspaces each, records stored only at their own endpoint, ids of other repositories and
workspaces, conditional updates, least privilege, a repository without a workspace, a workspace
that is not ready and workspace deletion, for reads and for branch protection.
`...TheOneShotCopyMovesForgeRecordsIntoTheirRepositorysWorkspace` and
`PostgresForgeRecords.MigrationRecordsEachRowsOwnerForTheCopy` cover the copy and migration 0054.
The `PostgresForgeScenario` case exercises the Liquid forge client, cursor movement, approval,
publication, and graph evidence through a real Lake server with a workspace Zetta.

## Change

Add a forge kind to `control::forge_record_kinds` and the capabilities that use it. A body change
that is not additive raises `ForgeRecordCodec`'s schema version
([schema evolution](../architecture/workspace-records.md#schema-evolution)).

## Migration and cutover (lake#341)

Migration `0054_forge_record_workspaces` records each `forge_records` row's owner in
`workspace_assignments`: `repository_workspace` (its repository's workspace), `needs_selection`
(the repository has no workspace yet; resolved by the copy once it has one) or `workspace_deleted`
(the workspace is gone, and the row with it; it does not hold the copy back). It adds no column and
drops nothing. The dev cutover is in
[workspace-records.md](../architecture/workspace-records.md#cutover-runbook-forge_records-dev-cluster).
